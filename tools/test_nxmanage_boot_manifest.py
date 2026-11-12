#!/usr/bin/env python3
"""Fail-closed verification of NX-CORE boot ownership and main image manifest."""
from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parents[1]
CORE_CONFIG = ROOT / "kernel/config"
BOOT = ROOT / "user/NexsOS1-main/sys/boot"
USERKIT = ROOT / "user/USERKIT"
MAX_SLOTS = 16
MAX_PAYLOAD = 64


def fail(reason: str) -> int:
    print(f"FAIL NXMANAGE manifest: {reason}")
    return 1


def load_images() -> dict[str, tuple[str, str]]:
    images: dict[str, tuple[str, str]] = {}
    for number, raw in enumerate((BOOT / "images.conf").read_text().splitlines(), 1):
        if not raw or raw.startswith("#"):
            continue
        fields = raw.split("|")
        if len(fields) != 3:
            raise ValueError(f"images.conf line {number}: expected three fields")
        kind, image, path = fields
        if kind not in {"init", "service"}:
            raise ValueError(f"images.conf line {number}: invalid image class")
        if not re.fullmatch(r"[a-z][a-z0-9_-]{0,22}", image) or image in images:
            raise ValueError(f"images.conf line {number}: invalid/duplicate image identity")
        if kind == "init" and not re.fullmatch(r"/sbin/[A-Za-z0-9_-]+", path):
            raise ValueError(f"images.conf line {number}: invalid init installation path")
        if kind == "service" and not re.fullmatch(r"/sys/(?:bin|services)/[A-Za-z0-9_-]+", path):
            raise ValueError(f"images.conf line {number}: invalid service installation path")
        images[image] = (kind, path)
    return images


def source_for(path: str) -> Path:
    name = Path(path).name
    if path.startswith("/sys/services/"):
        return USERKIT / "services" / name / "src/main.c"
    return ROOT / "user/NexsOS1-main/sys/bin" / f"{name}.c"


def main() -> int:
    if (CORE_CONFIG / "boot-entry.conf").read_text().strip() != "/krnl/extension/nxmanageDD":
        return fail("kernel-owned bootstrap entry changed without audit")
    if (BOOT / "entry").exists():
        return fail("userland must not duplicate the kernel boot entry")
    if (BOOT / "manager.conf").read_text().strip() != "init=nxinit":
        return fail("manager image identity contract changed without audit")
    if not (ROOT / "kernel/user/extension/nxmanageDD/main.c").is_file():
        return fail("kernel-owned manager source is missing")
    if not (USERKIT / "sbin/nxinit/src/main.c").is_file():
        return fail("NX-USERKIT init source is missing")

    try:
        images = load_images()
    except ValueError as exc:
        return fail(str(exc))
    if images.get("nxinit") != ("init", "/sbin/nxinit"):
        return fail("nxinit image is not catalogued as the selected init")

    catalog = json.loads((USERKIT / "catalog.json").read_text())
    userkit_projects = {entry["project"] for entry in catalog.get("services", [])}
    records = []
    seen = set()
    seen_names = set()
    manifest_userkit = set()
    for number, raw in enumerate((BOOT / "services.conf").read_text().splitlines(), 1):
        if not raw or raw.startswith("#"):
            continue
        fields = raw.split("|")
        if len(fields) not in (5, 6):
            return fail(f"line {number}: expected five or six fields")
        name, mode, image, arguments, key = fields[:5]
        authority = fields[5] if len(fields) == 6 else ""
        if authority not in ("", "window-manage"):
            return fail(f"line {number}: unknown authority")
        if authority and arguments:
            return fail(f"line {number}: argv-aware capability spawn unavailable")
        if authority and name != "pointer":
            return fail(f"line {number}: unexpected privileged service")
        if not re.fullmatch(r"[a-z][a-z0-9_-]{0,22}", name) or name in seen_names:
            return fail(f"line {number}: invalid/duplicate canonical name")
        seen_names.add(name)
        if mode not in {"lock", "once", "start", "debug"}:
            return fail(f"line {number}: unrecognized lifecycle mode")
        if image not in images or images[image][0] != "service":
            return fail(f"line {number}: unknown/non-service image identity")
        path = images[image][1]
        if key and not re.fullmatch(r"srv\.[A-Za-z0-9_.-]+", key):
            return fail(f"line {number}: registry key outside allowed namespace")
        if len(arguments.split()) > 2 or len(image) + len(arguments) + len(key) + 3 > MAX_PAYLOAD:
            return fail(f"line {number}: message exceeds existing PORT payload")
        source = source_for(path)
        if not source.is_file():
            return fail(f"line {number}: image has no owned source: {source}")
        if path.startswith("/sys/services/"):
            manifest_userkit.add(Path(path).name)
        identity = (image, arguments)
        if identity in seen:
            return fail(f"line {number}: duplicate image/args registration")
        seen.add(identity)
        records.append((name, mode, image, arguments, key))
        if name == "pointer" and authority != "window-manage":
            return fail("pointer service must explicitly receive window-manage")

    if manifest_userkit != userkit_projects:
        return fail("manifest USERKIT service set differs from NX-USERKIT catalog")
    if len(records) == 0 or len(records) > MAX_SLOTS:
        return fail(f"invalid slot count {len(records)}")
    required = {
        ("shell", "start", "shell", "/sys/bin/nxshell", ""),
        ("environment", "once", "environment", "", ""),
        ("tests", "debug", "tests", "-c /sys/bin/nxtestrunner", ""),
        ("roleprobe", "debug", "roleprobe", "", ""),
    }
    if not required.issubset(set(records)):
        return fail("required system-program lifecycle records are missing")
    if any((USERKIT / "services" / name).exists()
           for name in ("nxenvinit", "nxui", "nxbar", "nxlauncher", "nxshell")):
        return fail("ordinary system program incorrectly promoted to USERKIT")
    core = (ROOT / "kernel/main.c").read_text()
    for forbidden in ("/sbin/nxinit", "nxmanageDD", "nxtestrunner"):
        if forbidden in core:
            return fail(f"kernel composition root still knows userland name: {forbidden}")
    print(f"PASS: kernel-owned entry, {len(userkit_projects)} USERKIT services, "
          f"{len(records)} bounded image records")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
