#!/usr/bin/env python3
"""
nxrun.py — headless boot + drive the guest shell + report a verdict.

Python rewrite of tools/nxrun.sh with full QMP typing support.
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import List, Optional, Tuple


# ---------------------------------------------------------------------------
# QMP key typing (pure Python replacement of tools/qmp_type.py)
# ---------------------------------------------------------------------------

# Minimal QEMU QCode mapping used by the original harness.
# Enough for alphanumeric + common symbols + Enter.
QCODE = {
    " ": "spc",
    "\n": "ret",
    "\t": "tab",
    "-": "minus",
    "=": "equal",
    "[": "bracket_left",
    "]": "bracket_right",
    "\\": "backslash",
    ";": "semicolon",
    "'": "apostrophe",
    ",": "comma",
    ".": "dot",
    "/": "slash",
    "`": "grave_accent",
}

# Letters and digits are themselves
for c in "abcdefghijklmnopqrstuvwxyz0123456789":
    QCODE[c] = c

# Shifted characters
SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5",
    "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
    "_": "minus", "+": "equal", "{": "bracket_left",
    "}": "bracket_right", "|": "backslash", ":": "semicolon",
    '"': "apostrophe", "<": "comma", ">": "dot", "?": "slash",
    "~": "grave_accent",
}


def qmp_send(sock: socket.socket, cmd: dict) -> dict:
    """Send one QMP command and return the reply object."""
    import json
    data = (json.dumps(cmd) + "\r\n").encode()
    sock.sendall(data)
    # Read until we get a complete JSON object
    buf = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
        if b"\n" in buf:
            line, _, rest = buf.partition(b"\n")
            try:
                return json.loads(line.decode())
            except json.JSONDecodeError:
                continue
    return {}


def qmp_type(sock_path: str, text: str, delay: float = 0.02) -> None:
    """
    Type `text` into the guest via QMP input-send-event.
    A trailing newline is expected to be present in `text` when needed.
    """
    import json

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(sock_path)
        # Read the greeting
        s.recv(4096)
        # Negotiate capabilities
        qmp_send(s, {"execute": "qmp_capabilities"})

        for ch in text:
            events = []
            if ch in SHIFTED:
                # Press Shift
                events.append({
                    "type": "key",
                    "data": {
                        "down": True,
                        "key": {"type": "qcode", "data": "shift"}
                    }
                })
                qcode = SHIFTED[ch]
            else:
                qcode = QCODE.get(ch.lower() if ch.isalpha() else ch, None)
                if qcode is None:
                    # Fallback: skip unknown characters
                    continue

            # Key down
            events.append({
                "type": "key",
                "data": {
                    "down": True,
                    "key": {"type": "qcode", "data": qcode}
                }
            })
            # Key up
            events.append({
                "type": "key",
                "data": {
                    "down": False,
                    "key": {"type": "qcode", "data": qcode}
                }
            })

            if ch in SHIFTED:
                events.append({
                    "type": "key",
                    "data": {
                        "down": False,
                        "key": {"type": "qcode", "data": "shift"}
                    }
                })

            qmp_send(s, {
                "execute": "input-send-event",
                "arguments": {"events": events}
            })
            time.sleep(delay)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def is_test(cmd: str) -> bool:
    """A command is a test if its first word ends with 'test'."""
    first = cmd.split(maxsplit=1)[0]
    return first.endswith("test")


def expected_total(name: str, expect: List[str]) -> Optional[int]:
    for e in expect:
        if e.startswith(f"{name}="):
            try:
                return int(e.split("=", 1)[1])
            except ValueError:
                return None
    return None


def done_lines(name: str, log: Path) -> int:
    if not log.exists():
        return 0
    try:
        text = log.read_text(errors="replace")
        return len(re.findall(rf"{re.escape(name)}\] done:", text))
    except Exception:
        return 0


def kill_qemu(qemu_bin: str) -> None:
    try:
        subprocess.run(
            ["pkill", "-f", qemu_bin],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except Exception:
        pass
    time.sleep(1)


# ---------------------------------------------------------------------------
# Main logic
# ---------------------------------------------------------------------------

def run_one(
    arch: str,
    root: Path,
    cmds: List[str],
    expect: List[str],
    run_idx: int,
    total_runs: int,
) -> Tuple[bool, str]:
    """
    Execute a single run. Returns (success, summary_line).
    """
    B = root / "build" / arch
    disk = B / "disk.img"

    if not disk.is_file():
        return False, f"run{run_idx}/{arch}: missing {disk} — build first"

    # Architecture-specific QEMU setup
    if arch == "amd64":
        qemu = "qemu-system-x86_64"
        machine = ["-m", "5G", "-smp", "4"]
        devs = [
            "-vga", "none",
            "-device", "virtio-gpu-pci,disable-legacy=on,disable-modern=off",
            "-device", "virtio-keyboard-pci,disable-legacy=on,disable-modern=off",
            "-device", "virtio-mouse-pci,disable-legacy=on,disable-modern=off",
            "-drive", f"if=none,file={disk},id=hd0,format=raw",
            "-device", "virtio-blk-pci,drive=hd0,disable-legacy=on,disable-modern=off",
            "-kernel", str(B / "kernel.elf"),
        ]
        boot_wait = 25
        test_wait = 120
    else:  # aarch64
        qemu = "qemu-system-aarch64"
        machine = ["-M", "virt", "-cpu", "cortex-a57", "-m", "5G", "-smp", "4"]
        dtb = B / "virt.dtb"
        # Build DTB if missing
        if not dtb.is_file():
            r = subprocess.run(
                ["make", f"ARCH={arch}", str(dtb)],
                cwd=root,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            if r.returncode != 0:
                return False, f"run{run_idx}/{arch}: cannot build {dtb}"
        devs = [
            "-device", "virtio-gpu-device",
            "-device", "virtio-keyboard-device",
            "-device", "virtio-mouse-device",
            "-drive", f"if=none,file={disk},id=hd0,format=raw",
            "-device", "virtio-blk-device,drive=hd0",
            "-dtb", str(dtb),
            "-kernel", str(B / "kernel.bin"),
        ]
        boot_wait = 45
        test_wait = 300

    kill_qemu(qemu)

    with tempfile.TemporaryDirectory(prefix="nxr.", dir="/tmp") as tmp:
        tmp = Path(tmp)
        ser_log = tmp / "ser.log"
        qmp_sock = tmp / "qmp.sock"
        qemu_err = tmp / "qemu.err"

        # Clean environment variables that can interfere with QEMU/GTK
        env = os.environ.copy()
        for var in (
            "GTK_PATH", "GTK_EXE_PREFIX", "GTK_MODULES", "GTK_IM_MODULE_FILE",
            "GIO_MODULE_DIR", "GSETTINGS_SCHEMA_DIR", "XDG_DATA_DIRS",
            "XDG_DATA_HOME", "LOCPATH",
        ):
            env.pop(var, None)

        cmd = [
            qemu,
            *machine,
            "-serial", f"file:{ser_log}",
            "-display", "none",
            "-qmp", f"unix:{qmp_sock},server,nowait",
            *devs,
        ]

        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=open(qemu_err, "w"),
            env=env,
        )

        try:
            # ---- Wait for boot ----
            booted = False
            for _ in range(boot_wait * 2):
                if ser_log.exists():
                    try:
                        if "Entering supervisor loop" in ser_log.read_text(errors="replace"):
                            booted = True
                            break
                    except Exception:
                        pass
                time.sleep(0.5)

            if not booted:
                err = ""
                if qemu_err.exists():
                    err = qemu_err.read_text(errors="replace")[:200]
                return False, (
                    f"run{run_idx}/{arch}: NO BOOT — {err}\n"
                    f"  log: {ser_log}"
                )

            # ---- Type commands ----
            for c in cmds:
                name = c.split(maxsplit=1)[0]
                before = done_lines(name, ser_log)

                # Type the command + newline
                try:
                    qmp_type(str(qmp_sock), c + "\n")
                except Exception as e:
                    return False, f"run{run_idx}/{arch}: qmp_type failed: {e}"

                if is_test(c):
                    # Wait for evidence of completion
                    for _ in range(test_wait):
                        if done_lines(name, ser_log) > before:
                            break
                        time.sleep(1)
                else:
                    time.sleep(20)

            # ---- Collect results ----
            log_text = ser_log.read_text(errors="replace") if ser_log.exists() else ""

            faults = len(re.findall(
                r"KERNEL PANIC|KERNEL PAGE FAULT|GENERAL PROTECTION FAULT|Unrecoverable kernel",
                log_text,
            ))
            heap = len(re.findall(r"Invalid magic", log_text))

            verdict = "ok"
            why = ""
            if faults > 0 or heap > 0:
                verdict = "FAULT"
                why = f"faults={faults} heap={heap}"

            results = []
            for c in cmds:
                if not is_test(c):
                    continue
                name = c.split(maxsplit=1)[0]
                matches = re.findall(rf"{re.escape(name)}\] done: (\d+)/(\d+)", log_text)
                if not matches:
                    results.append(f"{name}=NO-RESULT")
                    if verdict != "FAULT":
                        verdict = "FAIL"
                    continue

                pass_n, total = map(int, matches[-1])  # last occurrence
                results.append(f"{name}={pass_n}/{total}")

                exp = expected_total(name, expect)
                if total == 0 or pass_n != total:
                    if verdict != "FAULT":
                        verdict = "FAIL"
                elif exp is not None and total != exp:
                    results[-1] += f"(expected {exp})"
                    if verdict != "FAULT":
                        verdict = "FAIL"

            summary = (
                f"run{run_idx}/{arch}: {verdict}"
                + (" " + " ".join(results) if results else "")
                + f" faults={faults} heap={heap}"
                + (f" ({why})" if why else "")
            )
            if verdict != "ok":
                summary += f"\n  log: {ser_log}"

            success = verdict == "ok"
            return success, summary

        finally:
            # Always kill this QEMU instance
            try:
                proc.send_signal(signal.SIGTERM)
                proc.wait(timeout=3)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
            kill_qemu(qemu)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="nxrun.py — headless QEMU test harness",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                          # single smoke boot (aarch64)
  %(prog)s -a amd64 -n 3            # three smoke runs on amd64
  %(prog)s -c captest -c libctest   # full gate
  %(prog)s -e captest=64 -e libctest=18 -c captest -c libctest
""",
    )
    parser.add_argument("-a", "--arch", default=os.environ.get("ARCH", "aarch64"),
                        choices=["amd64", "aarch64"],
                        help="architecture (default: $ARCH or aarch64)")
    parser.add_argument("-n", "--runs", type=int, default=1,
                        help="number of consecutive runs (default: 1)")
    parser.add_argument("-c", "--cmd", action="append", default=[],
                        dest="cmds",
                        help="command to type into the guest (repeatable)")
    parser.add_argument("-e", "--expect", action="append", default=[],
                        help="pin expected total, e.g. -e captest=64")

    args = parser.parse_args()

    # Move to project root (same as the bash script)
    script_dir = Path(__file__).resolve().parent
    root = script_dir.parent if script_dir.name == "tools" else script_dir
    os.chdir(root)

    rc = 0
    for i in range(1, args.runs + 1):
        ok, summary = run_one(
            arch=args.arch,
            root=root,
            cmds=args.cmds,
            expect=args.expect,
            run_idx=i,
            total_runs=args.runs,
        )
        print(summary)
        if not ok:
            rc = 1

    # Final cleanup
    kill_qemu("qemu-system-x86_64")
    kill_qemu("qemu-system-aarch64")
    return rc


if __name__ == "__main__":
    sys.exit(main())