#!/usr/bin/env python3
"""Verify actual service source, linked image classes and SDK client ownership.

This verifies build structure and ELF bytes, not runtime lifecycle behavior.
"""
from pathlib import Path
import argparse
import re

ROOT = Path(__file__).resolve().parents[1]
MACHINE = {"nxmanageDD"}
SBIN = {"nxinit"}
SERVERS = {"nxntfy_srv", "nxexecd", "nxtimerdd", "nxpaxd",
           "nxcompositor", "nxkeyboardd", "nxmoused", "nxttyd"}
SDK_CLIENTS = {"port_client", "gfx_buffer", "buffer_client", "execsvc_client", "notify_client",
               "surface_client", "time_client"}


def require(ok, description):
    if not ok:
        raise ValueError(description)


def make_set(lines, var):
    for i, line in enumerate(lines):
        if not line.startswith(var + " ="):
            continue
        text = line.partition("=")[2].strip()
        while text.endswith("\\") and i + 1 < len(lines):
            i += 1
            text = text[:-1] + " " + lines[i].strip()
        return set(re.findall(r"\$\(BUILD_DIR\)/([a-zA-Z0-9_]+)\.elf", text))
    raise ValueError("missing Makefile variable: " + var)


def source_roles():
    source = (ROOT / "Makefile").read_text()
    lines = source.splitlines()
    machine = make_set(lines, "NX_MACHINE_EXTENSION_ELFS")
    initializers = make_set(lines, "SBIN_ELFS")
    servers = make_set(lines, "SYS_SERVICE_ELFS")
    commands = make_set(lines, "SYS_ELFS")
    require(machine == MACHINE, "machine executable set differs")
    require(initializers == SBIN, "initializer executable set differs")
    require(servers == SERVERS, "dedicated daemon executable set differs")
    require(not commands.intersection(servers), "command and daemon binaries overlap")
    require({"nxexec", "nxpax", "nxshell"}.issubset(commands),
            "missing command personalities")
    for name in SERVERS:
        path = ROOT / "user/sys/services" / (name + ".c")
        require(path.is_file(), "missing actual daemon source " + str(path))
        body = path.read_text()
        require(len(re.findall(r"\bint\s+main\s*\(", body)) == 1,
                "daemon lacks unique entrypoint " + name)
    for name in ("nxexec", "nxpax"):
        body = (ROOT / "user/sys/bin" / (name + ".c")).read_text()
        require("return nxexec_service()" not in body and
                "return nxpax_service()" not in body,
                "command can execute privileged server: " + name)
    require("execsvc_launch_owned" in
            (ROOT / "user/sys/services/nxexecd.c").read_text() and
            "execsvc_resolve_user_program" in
            (ROOT / "system/lib/extensions/services/execsvc_client.c").read_text(),
            "executor RPC lacks capability-owned creation and shared user resolver")
    require("system.manage.service.%s" in
            (ROOT / "kernel/user/extension/nxmanageDD/main.c").read_text(),
            "named REGKEY service records missing")
    sdk_manifest = (ROOT / "system/sdk.mk").read_text()
    module_names = set(
        re.findall(r"extensions/services/([a-z_]+)\.o", sdk_manifest))
    require(module_names == SDK_CLIENTS, "SDK client linker set differs")
    require("USER_LIB_O := $(NXSDK_LIB_OBJECTS)" in source,
            "superproject does not consume the canonical NX-SDK object manifest")
    for name in SDK_CLIENTS:
        path = ROOT / "system/lib/extensions/services" / (name + ".c")
        require(path.is_file() and not re.search(r"\bint\s+main\s*\(",
                path.read_text()), "SDK client exposes a daemon entrypoint: " + name)
    require(not re.search(r"\bint\s+main\s*\(",
            (ROOT / "system/lib/gfx/theme.c").read_text()),
            "SDK graphics library cannot be a daemon process")

    names = set()
    service_names = set()
    for raw in (ROOT / "user/sys/boot/services.conf").read_text().splitlines():
        if not raw or raw.startswith("#"):
            continue
        parts = raw.split("|")
        require(len(parts) in (5, 6), "manifest must have 5 or 6 fields")
        name, mode, path, args, key = parts[:5]
        require(name not in names, "duplicate service identity " + name)
        names.add(name)
        if path.startswith("/sys/services/"):
            service_names.add(path.removeprefix("/sys/services/"))
    require(service_names == SERVERS, "actual server set differs from manifest")
    return len(names)


def rootfs_roles(directory):
    expected = {f"krnl/extension/{name}" for name in MACHINE}
    expected |= {f"sbin/{name}" for name in SBIN}
    expected |= {f"sys/services/{name}" for name in SERVERS}
    expected |= {"sys/bin/nxexec", "sys/bin/nxpax", "sys/bin/nxshell"}
    for relative in sorted(expected):
        path = directory / relative
        require(path.is_file(), "missing runtime executable: " + relative)
        require(path.open("rb").read(4) == b"\x7fELF",
                "runtime executable is not an ELF: " + relative)
    forbidden = ("sbin/nxmanageDD", "sys/bin/nxntfy_srv", "sys/bin/nxpaxd",
                 "sys/bin/nxexecd", "sys/bin/nxcompositor",
                 "sys/bin/nxkeyboardd", "sys/bin/nxmoused", "sys/bin/nxttyd")
    for relative in forbidden:
        require(not (directory / relative).exists(),
                "obsolete duplicate executable: " + relative)
    require((directory / "system/boot/entry").read_text().strip() ==
            "/krnl/extension/nxmanageDD", "machine extension VFS entry differs")
    require((directory / "system/boot/services.conf").read_bytes() ==
            (ROOT / "user/sys/boot/services.conf").read_bytes(),
            "boot service policy not installed from current source")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rootfs", type=Path)
    args = parser.parse_args()
    try:
        count = source_roles()
        if args.rootfs:
            rootfs_roles(args.rootfs)
    except (ValueError, OSError) as ex:
        print("FAIL NX-SERVICE: " + str(ex))
        return 1
    print(f"PASS: {count} named services, {len(SERVERS)} distinct server entrypoints, "
          f"{len(SDK_CLIENTS)} SDK client modules, distinct machine/init/ROOT/command outputs" +
          (" and real rootfs ELF/manifest bytes" if args.rootfs else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
