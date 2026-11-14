#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parents[1]
source = (ROOT / "sys/bin/demo1shell.c").read_text()
manifest = (ROOT / "sys/boot/services.conf").read_text()
manager = (ROOT / "sys/boot/manager.conf").read_text()
image_catalog = (REPO / "kernel/config/user-projects.conf").read_text()

need_source = [
    "NXShell:demo1>", "system.manage.service.%s",
    "system.init.name", "NX_example_echo", "example.notify.last",
]
need_manifest = [
    "notify|lock|/sys/services/nxnotify_registry",
    "exec|lock|/sys/services/nxexecd",
    "timer|lock|/sys/services/nxtimerdd",
    "pax|lock|/sys/services/nxpaxd",
    "compositor|lock|/sys/services/nxcompositor",
    "keyboard|lock|/sys/services/nxkeyboardd",
    "pointer|lock|/sys/services/nxmoused",
    "echo|lock|/sys/services/nxexample_echo",
    "shell|start|/sys/services/nxttyd|--shell /sys/bin/demo1shell",
]

missing = [x for x in need_source if x not in source]
missing += [x for x in need_manifest if x not in manifest]
if "init=/sbin/nxdemo1init" not in manager:
    missing.append("init=/sbin/nxdemo1init")
if "demo1|NexsOS demo1|demo1.img|" not in image_catalog:
    missing.append("kernel image catalog: demo1.img")

if missing:
    print("demo1 project test FAIL: " + ", ".join(missing), file=sys.stderr)
    raise SystemExit(1)
print("demo1 project test PASS")
