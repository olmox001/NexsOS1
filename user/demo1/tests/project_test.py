#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "sys/bin/demo1shell.c").read_text()
manifest = (ROOT / "sys/boot/services.conf").read_text()
manager = (ROOT / "sys/boot/manager.conf").read_text()
need_source = ["NXShell:demo1>", "system.manage.current", "system.manage.service.%s", "system.init.name", "NX_example_echo", "example.notify.last"]
need_manifest = [
    "/sys/services/nxnotify_registry", "/sys/services/nxexecd", "/sys/services/nxtimerdd",
    "/sys/services/nxpaxd", "/sys/services/nxcompositor", "/sys/services/nxkeyboardd",
    "/sys/services/nxmoused", "/sys/services/nxexample_echo", "/sys/services/nxttyd",
    "/sys/bin/demo1shell",
]
missing = [x for x in need_source if x not in source]
missing += [x for x in need_manifest if x not in manifest]
if "init=/sbin/nxdemo1init" not in manager:
    missing.append("init=/sbin/nxdemo1init")
if missing:
    print("demo1 project test FAIL: " + ", ".join(missing), file=sys.stderr)
    raise SystemExit(1)
print("demo1 project test PASS")
