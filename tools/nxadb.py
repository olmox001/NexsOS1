#!/usr/bin/env python3
"""
nxadb — Primitive ADB for NeXs OS

Built on top of nxshell (especially nxshell -c).
Provides an ADB-like interface for the NeXs guest running under QEMU.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

STATE_DIR = Path("/tmp/nxadb")
DEFAULT_ARCH = os.environ.get("ARCH", "aarch64")

FAULT_RE = re.compile(
    r"KERNEL PANIC|KERNEL PAGE FAULT|GENERAL PROTECTION FAULT|"
    r"Unrecoverable kernel|Invalid magic"
)
SHELL_ALIVE_RE = re.compile(r"NXShell: Alive|NXShell PID")
PROMPT_RE = re.compile(r"NXShell:[^\n]*> ")

# ---------------------------------------------------------------------------
# QMP key mapping (full editing support)
# ---------------------------------------------------------------------------

QCODE = {
    " ": "spc", "\n": "ret", "\t": "tab",
    "-": "minus", "=": "equal",
    "[": "bracket_left", "]": "bracket_right",
    "\\": "backslash", ";": "semicolon", "'": "apostrophe",
    ",": "comma", ".": "dot", "/": "slash", "`": "grave_accent",
    "\b": "backspace",
    "\x7f": "delete",
}
for c in "abcdefghijklmnopqrstuvwxyz0123456789":
    QCODE[c] = c

SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5",
    "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
    "_": "minus", "+": "equal",
    "{": "bracket_left", "}": "bracket_right", "|": "backslash",
    ":": "semicolon", '"': "apostrophe",
    "<": "comma", ">": "dot", "?": "slash", "~": "grave_accent",
}

SPECIAL = {
    "up": "up", "down": "down", "left": "left", "right": "right",
    "home": "home", "end": "end",
    "backspace": "backspace", "delete": "delete",
    "ret": "ret", "tab": "tab",
}


def qmp_connect(sock_path: Path, timeout: float = 4.0) -> socket.socket:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(str(sock_path))
    s.recv(8192)  # greeting
    qmp_send(s, {"execute": "qmp_capabilities"})
    return s


def qmp_send(sock: socket.socket, cmd: dict) -> dict:
    sock.sendall((json.dumps(cmd) + "\r\n").encode())
    buf = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
        if b"\n" in buf:
            line, _, _ = buf.partition(b"\n")
            try:
                return json.loads(line.decode())
            except json.JSONDecodeError:
                continue
    return {}


def qmp_key(sock: socket.socket, qcode: str, down: bool = True) -> None:
    qmp_send(sock, {
        "execute": "input-send-event",
        "arguments": {
            "events": [{
                "type": "key",
                "data": {
                    "down": down,
                    "key": {"type": "qcode", "data": qcode}
                }
            }]
        }
    })


def qmp_type(sock_path: Path, text: str, delay: float = 0.025) -> None:
    """
    Robust typing:
    - Always force lowercase for letters
    - Explicitly release Shift after every key
    - Higher delay to avoid dropped keys
    """
    with qmp_connect(sock_path) as s:
        # Ensure Shift is not stuck
        qmp_key(s, "shift", False)
        qmp_key(s, "shift_r", False)
        time.sleep(0.01)

        for ch in text:
            if ch in SHIFTED:
                qmp_key(s, "shift", True)
                time.sleep(0.008)
                qcode = SHIFTED[ch]
                qmp_key(s, qcode, True)
                time.sleep(0.008)
                qmp_key(s, qcode, False)
                qmp_key(s, "shift", False)
            else:
                if ch.isalpha():
                    qcode = ch.lower()
                else:
                    qcode = QCODE.get(ch)
                    if not qcode:
                        continue
                qmp_key(s, qcode, True)
                time.sleep(0.008)
                qmp_key(s, qcode, False)

            time.sleep(delay)

        # Final safety release
        qmp_key(s, "shift", False)
        qmp_key(s, "shift_r", False)


def qmp_special(sock_path: Path, name: str) -> None:
    qcode = SPECIAL.get(name)
    if not qcode:
        return
    with qmp_connect(sock_path) as s:
        qmp_key(s, qcode, True)
        time.sleep(0.015)
        qmp_key(s, qcode, False)
        time.sleep(0.02)


# ---------------------------------------------------------------------------
# Session
# ---------------------------------------------------------------------------

@dataclass
class Session:
    arch: str
    root: Path
    tmp: Path
    ser_log: Path
    qmp_sock: Path
    qemu_err: Path
    pid_file: Path
    qemu_bin: str = ""

    @classmethod
    def create(cls, arch: str, root: Path) -> "Session":
        STATE_DIR.mkdir(parents=True, exist_ok=True)
        tmp = STATE_DIR / arch
        tmp.mkdir(parents=True, exist_ok=True)
        return cls(
            arch=arch,
            root=root,
            tmp=tmp,
            ser_log=tmp / "ser.log",
            qmp_sock=tmp / "qmp.sock",
            qemu_err=tmp / "qemu.err",
            pid_file=tmp / "qemu.pid",
        )

    def is_running(self) -> bool:
        if not self.pid_file.exists():
            return False
        try:
            os.kill(int(self.pid_file.read_text().strip()), 0)
            return True
        except Exception:
            return False

    def pid(self) -> Optional[int]:
        try:
            return int(self.pid_file.read_text().strip())
        except Exception:
            return None

    def log_text(self) -> str:
        if self.ser_log.exists():
            return self.ser_log.read_text(errors="replace")
        return ""

    def log_size(self) -> int:
        return self.ser_log.stat().st_size if self.ser_log.exists() else 0


# ---------------------------------------------------------------------------
# QEMU lifecycle
# ---------------------------------------------------------------------------

def kill_all_qemu() -> None:
    for name in ("qemu-system-x86_64", "qemu-system-aarch64"):
        subprocess.run(
            ["pkill", "-f", name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    time.sleep(0.6)


def build_qemu_cmdline(session: Session) -> List[str]:
    B = session.root / "build" / session.arch
    disk = B / "disk.img"
    if not disk.is_file():
        raise FileNotFoundError(f"missing {disk} — build first")

    if session.arch == "amd64":
        session.qemu_bin = "qemu-system-x86_64"
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
    else:
        session.qemu_bin = "qemu-system-aarch64"
        machine = ["-M", "virt", "-cpu", "cortex-a57", "-m", "5G", "-smp", "4"]
        dtb = B / "virt.dtb"
        if not dtb.is_file():
            r = subprocess.run(
                ["make", f"ARCH={session.arch}", str(dtb)],
                cwd=session.root,
                capture_output=True,
            )
            if r.returncode != 0:
                raise RuntimeError(f"cannot build {dtb}")
        devs = [
            "-device", "virtio-gpu-device",
            "-device", "virtio-keyboard-device",
            "-device", "virtio-mouse-device",
            "-drive", f"if=none,file={disk},id=hd0,format=raw",
            "-device", "virtio-blk-device,drive=hd0",
            "-dtb", str(dtb),
            "-kernel", str(B / "kernel.bin"),
        ]

    return [
        session.qemu_bin, *machine,
        "-serial", f"file:{session.ser_log}",
        "-display", "none",
        "-qmp", f"unix:{session.qmp_sock},server,nowait",
        *devs,
    ]


def start_qemu(session: Session, wait: bool = True) -> None:
    if session.is_running():
        print(f"[nxadb] already running (pid {session.pid()})")
        return

    kill_all_qemu()
    for p in (session.ser_log, session.qemu_err, session.qmp_sock):
        p.unlink(missing_ok=True)

    cmd = build_qemu_cmdline(session)
    env = os.environ.copy()
    for v in (
        "GTK_PATH", "GTK_EXE_PREFIX", "GTK_MODULES", "GTK_IM_MODULE_FILE",
        "GIO_MODULE_DIR", "GSETTINGS_SCHEMA_DIR", "XDG_DATA_DIRS",
        "XDG_DATA_HOME", "LOCPATH",
    ):
        env.pop(v, None)

    with open(session.qemu_err, "w") as err:
        proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=err, env=env
        )
    session.pid_file.write_text(str(proc.pid))
    print(f"[nxadb] QEMU started ({session.arch}, pid {proc.pid})")

    if wait:
        wait_for_boot(session)
        wait_for_shell(session)


def stop_qemu(session: Session) -> None:
    pid = session.pid()
    if pid:
        try:
            os.kill(pid, signal.SIGTERM)
            time.sleep(0.9)
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    kill_all_qemu()
    session.pid_file.unlink(missing_ok=True)
    print("[nxadb] stopped")


def wait_for_boot(session: Session, timeout: int = 90) -> bool:
    print("[nxadb] waiting for kernel...", end="", flush=True)
    t0 = time.time()
    while time.time() - t0 < timeout:
        text = session.log_text()
        if "Entering supervisor loop" in text:
            print(" ok")
            return True
        if FAULT_RE.search(text):
            print(" FAULT")
            return False
        time.sleep(0.3)
        print(".", end="", flush=True)
    print(" TIMEOUT")
    return False


def wait_for_shell(session: Session, timeout: int = 50) -> bool:
    print("[nxadb] waiting for NXShell...", end="", flush=True)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if SHELL_ALIVE_RE.search(session.log_text()):
            print(" ready")
            return True
        time.sleep(0.35)
        print(".", end="", flush=True)
    print(" (timeout – continuing)")
    return False


# ---------------------------------------------------------------------------
# Robust output capture (prompt-based)
# ---------------------------------------------------------------------------

def wait_for_prompt(session: Session, after_size: int, timeout: float = 25.0) -> str:
    """
    Wait until the log stabilises and a prompt reappears.
    Returns all new text since after_size (never truncates).
    """
    t0 = time.time()
    last_size = after_size
    stable = 0

    while time.time() - t0 < timeout:
        time.sleep(0.2)
        cur = session.log_size()
        if cur == last_size:
            stable += 1
            if stable >= 4:  # ~0.8 s of stability
                text = session.log_text()
                new = text[after_size:] if after_size < len(text) else text
                if PROMPT_RE.search(new) or len(new) > 0:
                    return new
        else:
            stable = 0
            last_size = cur

    text = session.log_text()
    return text[after_size:] if after_size < len(text) else text


def run_shell_c(session: Session, command: str, timeout: float = 30.0) -> Tuple[int, str]:
    """
    Execute via nxshell -c "…" using safe JSON quoting.
    Conservative error detection.
    """
    if not session.is_running():
        start_qemu(session)

    before = session.log_size()
    typed = f"nxshell -c {json.dumps(command)}\n"
    qmp_type(session.qmp_sock, typed)

    new = wait_for_prompt(session, before, timeout=timeout)

    code = 0
    lower = new.lower()
    if "unknown command:" in lower:
        code = 127
    elif FAULT_RE.search(new):
        code = 1
    elif ("cannot remove" in lower or "cannot list" in lower or
          "cannot read" in lower or "cannot write" in lower or
          "cannot create" in lower):
        code = 1
    return code, new


def run_raw(session: Session, line: str, timeout: float = 12.0) -> str:
    """Direct typing (used by interactive shell)."""
    before = session.log_size()
    qmp_type(session.qmp_sock, line if line.endswith("\n") else line + "\n")
    return wait_for_prompt(session, before, timeout=timeout)


# ---------------------------------------------------------------------------
# ASCII helper (QMP cannot send non-ASCII)
# ---------------------------------------------------------------------------

def _to_ascii(text: str) -> str:
    """Convert non-ASCII characters to closest ASCII equivalents."""
    replacements = {
        "à": "a", "á": "a", "â": "a", "ä": "a",
        "è": "e", "é": "e", "ê": "e", "ë": "e",
        "ì": "i", "í": "i", "î": "i", "ï": "i",
        "ò": "o", "ó": "o", "ô": "o", "ö": "o",
        "ù": "u", "ú": "u", "û": "u", "ü": "u",
        "ç": "c", "ñ": "n",
        "À": "A", "È": "E", "É": "E", "Ì": "I",
        "Ò": "O", "Ù": "U",
        "—": "-", "–": "-", "“": '"', "”": '"',
        "‘": "'", "’": "'", "…": "...",
    }
    for src, dst in replacements.items():
        text = text.replace(src, dst)
    return "".join(c if ord(c) < 128 else "?" for c in text)


# ---------------------------------------------------------------------------
# High-level commands
# ---------------------------------------------------------------------------

def cmd_shell(session: Session, command: Optional[str]) -> int:
    if command is not None:
        code, out = run_shell_c(session, command)
        for l in out.splitlines():
            if PROMPT_RE.search(l) or "nxshell -c" in l:
                continue
            print(l)
        return code

    if not session.is_running():
        start_qemu(session)

    print("[nxadb] interactive shell  (exit / Ctrl-D to quit)")
    print("         Supports: arrows, backspace, delete")
    print("-------------------------------------------------------")
    try:
        while True:
            try:
                line = input("nx> ")
            except EOFError:
                break
            if line.strip() in ("exit", "quit"):
                break
            out = run_raw(session, line)
            for l in out.splitlines():
                if PROMPT_RE.search(l):
                    continue
                print(l)
    except KeyboardInterrupt:
        print()
    return 0


def cmd_ls(session: Session, path: str = ".") -> int:
    code, out = run_shell_c(session, f"ls {path}")
    for l in out.splitlines():
        if not PROMPT_RE.search(l) and "nxshell -c" not in l:
            print(l)
    return code


def cmd_cat(session: Session, path: str) -> int:
    code, out = run_shell_c(session, f"cat {path}")
    for l in out.splitlines():
        if not PROMPT_RE.search(l) and "nxshell -c" not in l:
            print(l)
    return code


def cmd_pull(session: Session, remote: str, local: str) -> int:
    """pull = cat <remote> with cleaning"""
    print(f"[nxadb] pull {remote} → {local}")
    code, out = run_shell_c(session, f"cat {remote}", timeout=40)

    lines = []
    for l in out.splitlines():
        if PROMPT_RE.search(l):
            continue
        if l.strip().startswith("nxshell -c") or l.strip().startswith("cat "):
            continue
        lines.append(l)

    content = "\n".join(lines).rstrip() + "\n"
    Path(local).write_text(content, errors="replace")
    print(f"[nxadb] wrote {len(content)} bytes → {local}")
    return code


def cmd_push(session: Session, local: str, remote: str) -> int:
    """
    Robust push using only guest commands.
    Non-ASCII characters are automatically converted to ASCII.
    """
    p = Path(local)
    if not p.is_file():
        print(f"[nxadb] file not found: {local}", file=sys.stderr)
        return 1

    raw = p.read_text(errors="replace")
    data = _to_ascii(raw)

    if data != raw:
        print("[nxadb] warning: non-ASCII characters were converted to ASCII")

    print(f"[nxadb] push {local} ({len(data)} bytes) → {remote}")

    # Remove existing file
    run_shell_c(session, f"rm {remote}", timeout=8)

    # ----- Method 1: small file → nxwrite -----
    if len(data) < 350:
        flat = data.replace("\n", " ").replace('"', '\\"')
        code, out = run_shell_c(session, f"write {remote} {flat}")
        if code == 0:
            print("[nxadb] push completed (write method)")
            return 0
        print("[nxadb] write failed, falling back to line-by-line…")

    # ----- Method 2: line-by-line (most robust) -----
    run_shell_c(session, f"write {remote} ", timeout=6)

    lines = data.splitlines()
    total = len(lines)

    for i, line in enumerate(lines):
        if "'" not in line and "\\" not in line:
            cmd = f"echo '{line}' >> {remote}"
        else:
            safe = (
                line.replace("\\", "\\\\")
                    .replace('"', '\\"')
                    .replace("$", "\\$")
                    .replace("`", "\\`")
            )
            cmd = f'echo "{safe}" >> {remote}'

        code, out = run_shell_c(session, cmd, timeout=12)

        if code != 0:
            print(f"[nxadb] error at line {i+1}/{total}:")
            print("--- output ---")
            print(out)
            print("--- command ---")
            print(cmd)
            return code

        if (i + 1) % 20 == 0 or (i + 1) == total:
            print(f"  … {i+1}/{total} lines")

    print(f"[nxadb] push completed ({total} lines)")
    return 0


def cmd_logcat(session: Session, follow: bool = True) -> None:
    if not session.ser_log.exists():
        print("[nxadb] no serial log yet")
        return
    with open(session.ser_log, "r", errors="replace") as f:
        sys.stdout.write(f.read())
        sys.stdout.flush()
        if not follow:
            return
        print("\n--- following (Ctrl-C to stop) ---")
        try:
            while True:
                line = f.readline()
                if line:
                    sys.stdout.write(line)
                    sys.stdout.flush()
                else:
                    time.sleep(0.1)
        except KeyboardInterrupt:
            print()


def cmd_status(session: Session) -> None:
    print(f"arch     : {session.arch}")
    print(f"running  : {session.is_running()}")
    if session.is_running():
        print(f"pid      : {session.pid()}")
        text = session.log_text()
        print(f"shell    : {'yes' if SHELL_ALIVE_RE.search(text) else 'no'}")
        print(f"faults   : {len(FAULT_RE.findall(text))}")
        print(f"log size : {session.log_size()} bytes")


def cmd_test(session: Session, cmds: List[str], expect: List[str], runs: int) -> int:
    rc = 0
    for i in range(1, runs + 1):
        print(f"\n=== run {i}/{runs} ({session.arch}) ===")
        stop_qemu(session)
        start_qemu(session)

        verdict = "ok"
        results = []

        for c in cmds:
            name = c.split()[0]
            before = len(re.findall(rf"{re.escape(name)}\] done:", session.log_text()))
            run_shell_c(session, c, timeout=180)
            if name.endswith("test"):
                for _ in range(200):
                    if len(re.findall(rf"{re.escape(name)}\] done:", session.log_text())) > before:
                        break
                    time.sleep(0.7)

        text = session.log_text()
        faults = len(FAULT_RE.findall(text))
        if faults:
            verdict = "FAULT"

        for c in cmds:
            if not c.split()[0].endswith("test"):
                continue
            name = c.split()[0]
            m = re.findall(rf"{re.escape(name)}\] done: (\d+)/(\d+)", text)
            if not m:
                results.append(f"{name}=NO-RESULT")
                if verdict != "FAULT":
                    verdict = "FAIL"
                continue
            p, t = map(int, m[-1])
            results.append(f"{name}={p}/{t}")
            if t == 0 or p != t:
                if verdict != "FAULT":
                    verdict = "FAIL"
            for e in expect:
                if e.startswith(f"{name}="):
                    try:
                        if t != int(e.split("=")[1]):
                            results[-1] += f"(expected {e.split('=')[1]})"
                            if verdict != "FAULT":
                                verdict = "FAIL"
                    except ValueError:
                        pass

        print(f"run{i}/{session.arch}: {verdict} {' '.join(results)} faults={faults}")
        if verdict != "ok":
            rc = 1
            print(f"  log → {session.ser_log}")
    return rc


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        prog="nxadb",
        description="Primitive ADB for NeXs OS (built on nxshell)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  nxadb start
  nxadb shell
  nxadb shell "ls /sys/bin"
  nxadb ls /home
  nxadb cat /home/.nxshell_history
  nxadb push README.md /home/README.md
  nxadb pull /home/README.md ./from-guest.md
  nxadb logcat
  nxadb test -c captest -c libctest
  nxadb stop
""",
    )
    parser.add_argument(
        "-a", "--arch", default=DEFAULT_ARCH, choices=["amd64", "aarch64"]
    )

    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("start", help="Boot the guest")
    sub.add_parser("stop", help="Stop the guest")
    sub.add_parser("restart", help="Restart the guest")
    sub.add_parser("status", help="Show status")
    sub.add_parser("wait-for-boot", help="Wait until kernel is ready")
    sub.add_parser("wait-for-shell", help="Wait until NXShell is ready")

    p_shell = sub.add_parser("shell", help="Interactive or one-shot shell")
    p_shell.add_argument("command", nargs="?", default=None)

    p_ls = sub.add_parser("ls", help="List directory")
    p_ls.add_argument("path", nargs="?", default=".")

    p_cat = sub.add_parser("cat", help="Print file contents")
    p_cat.add_argument("path")

    p_push = sub.add_parser("push", help="Copy host → guest")
    p_push.add_argument("local")
    p_push.add_argument("remote")

    p_pull = sub.add_parser("pull", help="Copy guest → host")
    p_pull.add_argument("remote")
    p_pull.add_argument("local")

    p_log = sub.add_parser("logcat", help="Show / follow serial log")
    p_log.add_argument("--no-follow", action="store_false", dest="follow")

    p_test = sub.add_parser("test", help="Full test gate")
    p_test.add_argument("-c", "--cmd", action="append", default=[], dest="cmds")
    p_test.add_argument("-e", "--expect", action="append", default=[])
    p_test.add_argument("-n", "--runs", type=int, default=1)

    args = parser.parse_args()

    script = Path(__file__).resolve()
    root = script.parent.parent if script.parent.name == "tools" else script.parent
    os.chdir(root)
    session = Session.create(args.arch, root)

    try:
        if args.cmd == "start":
            start_qemu(session)
        elif args.cmd == "stop":
            stop_qemu(session)
        elif args.cmd == "restart":
            stop_qemu(session)
            start_qemu(session)
        elif args.cmd == "status":
            cmd_status(session)
        elif args.cmd == "wait-for-boot":
            return 0 if wait_for_boot(session) else 1
        elif args.cmd == "wait-for-shell":
            return 0 if wait_for_shell(session) else 1
        elif args.cmd == "shell":
            return cmd_shell(session, args.command)
        elif args.cmd == "ls":
            return cmd_ls(session, args.path)
        elif args.cmd == "cat":
            return cmd_cat(session, args.path)
        elif args.cmd == "push":
            return cmd_push(session, args.local, args.remote)
        elif args.cmd == "pull":
            return cmd_pull(session, args.remote, args.local)
        elif args.cmd == "logcat":
            cmd_logcat(session, follow=args.follow)
        elif args.cmd == "test":
            return cmd_test(session, args.cmds, args.expect, args.runs)
    except KeyboardInterrupt:
        print("\n[nxadb] interrupted")
        return 130
    except Exception as e:
        print(f"[nxadb] error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())