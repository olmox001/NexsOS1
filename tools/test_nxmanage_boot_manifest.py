#!/usr/bin/env python3
"""Fail-closed verification of NX-CORE boot ownership and image startup policy."""
from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parents[1]
CORE_CONFIG = ROOT / "kernel/config"
BOOT = ROOT / "user/NexsOS1-main/sys/boot"
USERKIT = ROOT / "user/USERKIT"


def fail(reason: str) -> int:
    print(f"FAIL NXMANAGE manifest: {reason}")
    return 1


def source_for(path: str) -> Path:
    name = Path(path).name
    if path.startswith("/sys/services/"):
        return USERKIT / "services" / name / "src/main.c"
    return ROOT / "user/NexsOS1-main/sys/bin" / f"{name}.c"


def valid_exec(path: str) -> bool:
    return re.fullmatch(r"/sys/(?:bin|services)/[A-Za-z0-9_-]+", path) is not None


def main() -> int:
    if (CORE_CONFIG / "boot-entry.conf").read_text().strip() != "/krnl/extension/nxmanageDD":
        return fail("kernel-owned bootstrap entry changed without audit")
    if (BOOT / "entry").exists():
        return fail("userland must not duplicate the kernel boot entry")
    if (BOOT / "images.conf").exists():
        return fail("duplicate executable image catalog is forbidden")
    if (BOOT / "manager.conf").read_text().strip() != "init=/sbin/nxinit":
        return fail("selected image startup locator changed without audit")
    if not (ROOT / "kernel/user/extension/nxmanageDD/main.c").is_file():
        return fail("kernel-owned manager source is missing")
    if not (USERKIT / "sbin/nxinit/src/main.c").is_file():
        return fail("NX-USERKIT init source is missing")

    catalog = json.loads((USERKIT / "catalog.json").read_text())
    userkit_projects = {entry["project"] for entry in catalog.get("services", [])}
    records = []
    seen_names = set()
    seen_exec = set()
    manifest_userkit = set()

    for number, raw in enumerate((BOOT / "services.conf").read_text().splitlines(), 1):
        if not raw or raw.startswith("#"):
            continue
        fields = raw.split("|")
        if len(fields) not in (5, 6):
            return fail(f"line {number}: expected five or six fields")
        name, mode, executable, arguments, key = fields[:5]
        authority = fields[5] if len(fields) == 6 else ""
        if authority not in ("", "window-manage"):
            return fail(f"line {number}: unknown authority")
        if authority and arguments:
            return fail(f"line {number}: argv-aware capability spawn unavailable")
        if authority and name != "pointer":
            return fail(f"line {number}: unexpected privileged service")
        if not re.fullmatch(r"[a-z][a-z0-9_-]*", name) or name in seen_names:
            return fail(f"line {number}: invalid/duplicate canonical name")
        seen_names.add(name)
        if mode not in {"lock", "once", "start", "async", "debug"}:
            return fail(f"line {number}: unrecognized lifecycle mode")
        if not valid_exec(executable):
            return fail(f"line {number}: invalid startup executable")
        if key and not re.fullmatch(r"srv\.[A-Za-z0-9_.-]+", key):
            return fail(f"line {number}: registry key outside allowed namespace")
        source = source_for(executable)
        if not source.is_file():
            return fail(f"line {number}: executable has no owned source: {source}")
        if executable.startswith("/sys/services/"):
            manifest_userkit.add(Path(executable).name)
        identity = (executable, arguments)
        if identity in seen_exec:
            return fail(f"line {number}: duplicate executable/args declaration")
        seen_exec.add(identity)
        records.append((name, mode, executable, arguments, key))
        if name == "pointer" and authority != "window-manage":
            return fail("pointer service must explicitly receive window-manage")

    if manifest_userkit != userkit_projects:
        return fail("startup USERKIT service set differs from NX-USERKIT build set")
    if not records:
        return fail("startup declaration is empty")

    required = {
        ("shell", "start", "/sys/services/nxttyd", "--shell /sys/bin/nxshell", ""),
        ("environment", "once", "/sys/bin/nxenvinit", "", ""),
        ("tests", "async", "/sys/services/nxttyd", "/sys/bin/nxtestrunner", ""),
        ("roleprobe", "debug", "/sys/bin/nxroleprobe", "", ""),
    }
    if not required.issubset(set(records)):
        return fail("required system-program lifecycle records are missing")
    if any((USERKIT / "services" / name).exists()
           for name in ("nxenvinit", "nxui", "nxbar", "nxlauncher", "nxshell")):
        return fail("ordinary system program incorrectly promoted to USERKIT")

    runtime = (ROOT / "user/USERKIT/sbin/common/nxinit_runtime.c").read_text()
    if 'strcmp(field[1], "async") == 0' not in runtime or \
       "mode = NXMANAGE_MODE_ASYNC_ONCE;" not in runtime:
        return fail("nxinit runtime does not implement the async one-shot manifest mode")

    manager = (ROOT / "kernel/user/extension/nxmanageDD/main.c").read_text()
    for forbidden in ("images.conf", "nxsystem_image", "NXMANAGE_LIBRARY",
                      "system.manage.current", "file_read("):
        if forbidden in manager:
            return fail(f"manager contains forbidden policy/storage reference: {forbidden}")

    core = (ROOT / "kernel/main.c").read_text()
    for forbidden in ("/sbin/nxinit", "nxmanageDD", "nxtestrunner"):
        if forbidden in core:
            return fail(f"kernel composition root still knows userland name: {forbidden}")

    print(f"PASS: kernel-owned entry, {len(userkit_projects)} USERKIT services, "
          f"{len(records)} image-owned startup records")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
