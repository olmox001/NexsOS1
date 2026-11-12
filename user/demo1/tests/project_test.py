#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "sys/bin/demo1shell.c").read_text()
manifest = (ROOT / "sys/boot/services.conf").read_text()
images = (ROOT / "sys/boot/images.conf").read_text()
manager = (ROOT / "sys/boot/manager.conf").read_text()
need_source = [
    "NXShell:demo1>", "system.manage.current", "system.manage.service.%s",
    "system.init.name", "NX_example_echo", "example.notify.last",
]
need_manifest = [
    "notify|lock|notify", "exec|lock|exec", "timer|lock|timer", "pax|lock|pax",
    "compositor|lock|compositor", "keyboard|lock|keyboard",
    "pointer|lock|pointer", "echo|lock|echo", "shell|start|shell",
]
need_images = [
    "service|notify|/sys/services/nxnotify_registry",
    "service|exec|/sys/services/nxexecd",
    "service|timer|/sys/services/nxtimerdd",
    "service|pax|/sys/services/nxpaxd",
    "service|compositor|/sys/services/nxcompositor",
    "service|keyboard|/sys/services/nxkeyboardd",
    "service|pointer|/sys/services/nxmoused",
    "service|echo|/sys/services/nxexample_echo",
    "service|shell|/sys/services/nxttyd",
]
missing = [x for x in need_source if x not in source]
missing += [x for x in need_manifest if x not in manifest]
missing += [x for x in need_images if x not in images]
if "init=nxdemo1init" not in manager:
    missing.append("init=nxdemo1init")
if missing:
    print("demo1 project test FAIL: " + ", ".join(missing), file=sys.stderr)
    raise SystemExit(1)
print("demo1 project test PASS")
