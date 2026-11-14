#!/usr/bin/env python3
"""Verify NX-CORE manager, NX-USERKIT services, SDK extensions and image layout."""
from pathlib import Path
import argparse
import json
import re

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "user/NexsOS1-main"
USERKIT = ROOT / "user/USERKIT"
SDK = ROOT / "system"
CORE = ROOT / "kernel"
MACHINE = {"nxmanageDD"}
COMMANDS = {"nxexec", "nxpax", "nxshell", "nxenvinit", "nxui", "nxbar", "nxlauncher"}


def require(ok: bool, description: str) -> None:
    if not ok:
        raise ValueError(description)


def service_projects() -> set[str]:
    data = json.loads((USERKIT / "catalog.json").read_text())
    projects = {entry["project"] for entry in data.get("services", [])}
    require(projects, "NX-USERKIT catalog is empty")
    return projects


def source_roles() -> int:
    servers = service_projects()
    root_make = (ROOT / "Makefile").read_text()
    main_manifest = (MAIN / "userland.mk").read_text()
    require("include $(USER_DIR)/userland.mk" in root_make,
            "superproject does not consume selected userland manifest")
    require("NEXS_USERLAND_SERVICE_PROGRAMS :=" in main_manifest,
            "main userland service boundary variable missing")
    service_line = re.search(r"^NEXS_USERLAND_SERVICE_PROGRAMS[ \t]*:=[ \t]*(.*)$",
                             main_manifest, re.MULTILINE)
    require(service_line is not None and not service_line.group(1).strip(),
            "NexsOS1-main must not own USERKIT service implementations")

    for name in servers:
        source = USERKIT / "services" / name / "src/main.c"
        require(source.is_file(), f"missing USERKIT server source: {name}")
        require(len(re.findall(r"\bint\s+main\s*\(", source.read_text())) == 1,
                f"USERKIT server lacks unique entrypoint: {name}")
        extension = SDK / "lib/extensions/USERKIT" / name / "project.mk"
        require(extension.is_file(), f"missing SDK USERKIT extension: {name}")
        require("NXSDK_EXTENSION_CLASS := USERKIT" in extension.read_text(),
                f"SDK extension is not USERKIT-classed: {name}")

    for name in COMMANDS:
        source = MAIN / "sys/bin" / f"{name}.c"
        require(source.is_file(), f"system program moved out of NexsOS1-main: {name}")
        require(not (USERKIT / "services" / name).exists(),
                f"ordinary sys/bin program incorrectly promoted to USERKIT: {name}")

    manager = (CORE / "user/extension/nxmanageDD/main.c").read_text()
    require("system.manage.service.%s" in manager,
            "named manager service records missing")
    require("NXMANAGE_PROJECT_CONFIG" not in manager and
            "system.manage.current" not in manager,
            "machine manager must not contain build-project identity")
    require("[NXMANAGE] service=" in manager,
            "manager spawn UART diagnostics missing")
    require((CORE / "config/boot-entry.conf").read_text().strip() ==
            "/krnl/extension/nxmanageDD",
            "kernel boot entry does not select nxmanageDD")

    require(not (MAIN / "sys/boot/images.conf").exists(),
            "duplicate executable image catalog is forbidden")
    for forbidden in ("file_read(", "nxsystem_image", "NXMANAGE_LIBRARY",
                      "NXMANAGE_PROJECT", "system.manage.current"):
        require(forbidden not in manager,
                f"machine manager contains forbidden policy/storage reference: {forbidden}")

    manifest_servers = set()
    names = set()
    for raw in (MAIN / "sys/boot/services.conf").read_text().splitlines():
        if not raw or raw.startswith("#"):
            continue
        fields = raw.split("|")
        require(len(fields) in (5, 6), "startup declaration must have 5 or 6 fields")
        logical, _mode, executable = fields[:3]
        require(logical not in names, f"duplicate startup name: {logical}")
        require(re.fullmatch(r"/sys/(?:bin|services)/[A-Za-z0-9_-]+", executable)
                is not None, f"invalid startup executable: {executable}")
        names.add(logical)
        if executable.startswith("/sys/services/"):
            manifest_servers.add(executable.removeprefix("/sys/services/"))
    require(manifest_servers == servers,
            f"USERKIT build/startup mismatch: {manifest_servers} != {servers}")
    return len(names)


def rootfs_roles(directory: Path) -> None:
    servers = service_projects()
    expected = {"krnl/extension/nxmanageDD", "sbin/nxinit"}
    expected |= {f"sys/services/{name}" for name in servers}
    expected |= {f"sys/bin/{name}" for name in COMMANDS}
    for relative in sorted(expected):
        path = directory / relative
        require(path.is_file(), f"missing runtime executable: {relative}")
        require(path.read_bytes()[:4] == b"\x7fELF",
                f"runtime executable is not ELF: {relative}")
    for name in sorted(servers):
        probe = directory / "sys/bin" / f"nxtest_userkit_{name}"
        require(probe.is_file(), f"missing USERKIT runtime probe: {name}")
        require(probe.read_bytes()[:4] == b"\x7fELF",
                f"USERKIT runtime probe is not ELF: {name}")
    require(not (directory / "system/boot/entry").exists(),
            "rootfs must not own the NX-CORE boot entry")
    require(not (directory / "system/boot/images.conf").exists(),
            "rootfs must not contain a duplicate executable catalog")
    require((directory / "system/boot/project").read_text().strip() == "NexsOS1-main",
            "main image project identity differs")
    require((directory / "system/boot/services.conf").read_bytes() ==
            (MAIN / "sys/boot/services.conf").read_bytes(),
            "boot service policy not installed from NexsOS1-main")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rootfs", type=Path)
    args = parser.parse_args()
    try:
        count = source_roles()
        if args.rootfs:
            rootfs_roles(args.rootfs)
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        print("FAIL NX-SERVICE: " + str(exc))
        return 1
    print(f"PASS: {count} manager entries, {len(service_projects())} USERKIT services, "
          "system programs separated from service implementations" +
          (" and rootfs layout verified" if args.rootfs else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
