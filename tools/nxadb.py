#!/usr/bin/env python3
"""
nxadb — Primitive ADB for NeXs OS

Uses QMP keyboard input to control the active NXShell prompt in a QEMU guest.
Provides an ADB-like interface for guest commands, files, logs and test runs.
"""

from __future__ import annotations

import argparse
import fcntl
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

from nxqemu import build_qemu_command, clean_qemu_environment

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

PROJECT_ROOT = Path(__file__).resolve().parents[1]
STATE_DIR = PROJECT_ROOT / "build" / "runtime" / "nxadb"
QMP_DIR = PROJECT_ROOT / "build" / "q"
DEFAULT_ARCH = os.environ.get("ARCH", "aarch64")
QMP_INPUT_DISPLAY = "nxgpu0"
SESSION_LOCK_TIMEOUT = 60.0
MUTATING_COMMANDS = {"start", "stop", "restart", "shell", "ls", "cat",
                     "push", "pull", "test"}


class SessionBusyError(RuntimeError):
    """Another nxadb/nxrun process owns this architecture's guest session."""


FAULT_RE = re.compile(
    r"KERNEL PANIC|KERNEL PAGE FAULT|GENERAL PROTECTION FAULT|"
    r"Unrecoverable kernel|Invalid magic"
)
ANSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
SHELL_ALIVE_RE = re.compile(
    r"NXShell: Alive|NXShell PID|NXShell started \(PID|NXShell\] TTY Window .* active",
    re.IGNORECASE,
)
PROMPT_RE = re.compile(r"NXShell:[^\r\n]*>\s*", re.IGNORECASE)


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)

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


_QMP_BUFFERS: dict[socket.socket, bytearray] = {}
_QMP_NEXT_ID = 1


def _qmp_recv_json(sock: socket.socket) -> dict:
    buf = _QMP_BUFFERS.setdefault(sock, bytearray())
    while True:
        newline = buf.find(b"\n")
        if newline >= 0:
            line = bytes(buf[:newline]).strip()
            del buf[:newline + 1]
            if not line:
                continue
            return json.loads(line.decode("utf-8", errors="strict"))
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("QMP socket closed before a response")
        buf.extend(chunk)


def qmp_close(sock: socket.socket) -> None:
    _QMP_BUFFERS.pop(sock, None)
    try:
        sock.close()
    except OSError:
        pass


def qmp_send(sock: socket.socket, cmd: dict) -> dict:
    global _QMP_NEXT_ID
    request_id = _QMP_NEXT_ID
    _QMP_NEXT_ID += 1
    message = dict(cmd)
    message["id"] = request_id
    sock.sendall((json.dumps(message) + "\r\n").encode("utf-8"))
    while True:
        response = _qmp_recv_json(sock)
        # QMP events can arrive between a command and its response. Ignore
        # events and wait for this request's response instead of misreading it.
        if response.get("id") == request_id or "return" in response or "error" in response:
            return response


def qmp_connect(sock_path: Path, timeout: float = 4.0) -> socket.socket:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(str(sock_path))
        greeting = _qmp_recv_json(s)
        if "QMP" not in greeting:
            raise RuntimeError(f"invalid QMP greeting from {sock_path}")
        response = qmp_send(s, {"execute": "qmp_capabilities"})
        if "error" in response:
            raise RuntimeError(f"QMP capability negotiation failed: {response['error']}")
        return s
    except Exception:
        qmp_close(s)
        raise


def qmp_key(sock: socket.socket, qcode: str, down: bool = True) -> None:
    response = qmp_send(sock, {
        "execute": "input-send-event",
        "arguments": {
            "device": QMP_INPUT_DISPLAY,
            "events": [{
                "type": "key",
                "data": {
                    "down": down,
                    "key": {"type": "qcode", "data": qcode}
                }
            }]
        }
    })
    if "error" in response:
        raise RuntimeError(f"QMP keyboard input failed: {response['error']}")


def qmp_type(sock_path: Path, text: str, delay: float = 0.025) -> None:
    """Type ASCII text over QMP, including uppercase and shifted punctuation."""
    s = qmp_connect(sock_path)
    try:
        qmp_key(s, "shift", False)
        qmp_key(s, "shift_r", False)
        time.sleep(0.01)
        for ch in text:
            if ch in SHIFTED:
                qcode = SHIFTED[ch]
                qmp_key(s, "shift", True)
                qmp_key(s, qcode, True)
                qmp_key(s, qcode, False)
                qmp_key(s, "shift", False)
            else:
                if ch.isalpha():
                    qcode = ch.lower()
                    if ch.isupper():
                        qmp_key(s, "shift", True)
                else:
                    qcode = QCODE.get(ch)
                    if not qcode:
                        continue
                qmp_key(s, qcode, True)
                qmp_key(s, qcode, False)
                if ch.isalpha() and ch.isupper():
                    qmp_key(s, "shift", False)
            time.sleep(delay)
        qmp_key(s, "shift", False)
        qmp_key(s, "shift_r", False)
    finally:
        qmp_close(s)


def qmp_special(sock_path: Path, name: str) -> None:
    qcode = SPECIAL.get(name)
    if not qcode:
        return
    s = qmp_connect(sock_path)
    try:
        qmp_key(s, qcode, True)
        time.sleep(0.015)
        qmp_key(s, qcode, False)
        time.sleep(0.02)
    finally:
        qmp_close(s)


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
    vars_file: Path
    qemu_bin: str = ""

    @classmethod
    def create(cls, arch: str, root: Path) -> "Session":
        root = root.resolve()
        state_dir = root / "build" / "runtime" / "nxadb"
        state_dir.mkdir(parents=True, exist_ok=True)
        tmp = state_dir / arch
        tmp.mkdir(parents=True, exist_ok=True)
        qmp_dir = root / "build" / "q"
        qmp_dir.mkdir(parents=True, exist_ok=True)
        return cls(
            arch=arch,
            root=root,
            tmp=tmp,
            ser_log=tmp / "serial.log",
            qmp_sock=qmp_dir / f"{arch}.sock",
            qemu_err=tmp / "qemu.stderr.log",
            pid_file=tmp / "qemu.pid.json",
            vars_file=tmp / "uefi-vars.fd",
        )

    def pid(self) -> Optional[int]:
        try:
            raw = self.pid_file.read_text().strip()
            try:
                data = json.loads(raw)
                return int(data["pid"])
            except (json.JSONDecodeError, TypeError, KeyError):
                return int(raw)
        except (OSError, ValueError):
            return None

    def process_matches(self, pid: Optional[int]) -> bool:
        if not pid:
            return False
        try:
            os.kill(pid, 0)
        except (ProcessLookupError, PermissionError, OSError):
            return False
        result = subprocess.run(
            ["ps", "-p", str(pid), "-o", "command="],
            capture_output=True, text=True,
        )
        command = result.stdout.strip()
        return bool(command and str(self.qmp_sock) in command and "qemu-system-" in command)

    def is_running(self) -> bool:
        if self.qmp_sock.exists():
            try:
                sock = qmp_connect(self.qmp_sock, timeout=1.5)
                try:
                    response = qmp_send(sock, {"execute": "query-status"})
                    if "return" in response:
                        return True
                finally:
                    qmp_close(sock)
            except (OSError, RuntimeError, ValueError, ConnectionError, json.JSONDecodeError):
                pass
        return self.process_matches(self.pid())

    def boot_iso(self) -> Path:
        if self.arch == "amd64":
            return self.root / "build" / "amd64" / "grub-bios.iso"
        return self.root / "build" / "aarch64" / "grub-efi" / "nexsos-aarch64-uefi.iso"

    def disk_image(self) -> Path:
        return self.root / "build" / self.arch / "disk.img"

    def log_text(self) -> str:
        if self.ser_log.exists():
            return self.ser_log.read_text(errors="replace")
        return ""

    def log_size(self) -> int:
        # Offsets are used to slice decoded text, so measure characters rather
        # than UTF-8 file bytes (ANSI/Unicode output makes the two differ).
        return len(self.log_text())


# ---------------------------------------------------------------------------
# QEMU lifecycle
# ---------------------------------------------------------------------------

def build_qemu_cmdline(session: Session) -> List[str]:
    return build_qemu_command(
        session.arch,
        session.boot_iso(),
        session.disk_image(),
        serial=f"file:{session.ser_log}",
        display="none",
        qmp_socket=session.qmp_sock,
        uefi_vars_file=session.vars_file,
    )


def ensure_artifacts(session: Session) -> None:
    if session.boot_iso().is_file() and session.disk_image().is_file():
        return
    print(f"[nxadb] boot artifacts missing; building ARCH={session.arch} boot-iso")
    result = subprocess.run(
        ["make", f"ARCH={session.arch}", "boot-iso"],
        cwd=session.root,
    )
    if result.returncode != 0:
        raise RuntimeError(f"make ARCH={session.arch} boot-iso failed ({result.returncode})")
    if not session.boot_iso().is_file() or not session.disk_image().is_file():
        raise RuntimeError("boot-iso target completed without producing ISO and disk image")


def start_qemu(session: Session, wait: bool = True) -> None:
    if session.is_running():
        print(f"[nxadb] already running (pid {session.pid() or 'QMP-owned'})")
        return

    ensure_artifacts(session)
    session.tmp.mkdir(parents=True, exist_ok=True)
    session.qmp_sock.parent.mkdir(parents=True, exist_ok=True)
    for path in (session.ser_log, session.qemu_err, session.qmp_sock, session.pid_file):
        path.unlink(missing_ok=True)

    cmd = build_qemu_cmdline(session)
    session.qemu_bin = cmd[0]
    env = clean_qemu_environment()
    with session.qemu_err.open("w") as err:
        proc = subprocess.Popen(
            cmd, cwd=session.root, stdout=subprocess.DEVNULL, stderr=err,
            env=env, start_new_session=True,
        )
    session.pid_file.write_text(json.dumps({
        "pid": proc.pid,
        "arch": session.arch,
        "qmp_socket": str(session.qmp_sock),
        "qemu": session.qemu_bin,
    }, indent=2) + "\n")
    print(f"[nxadb] QEMU started via GRUB ({session.arch}, pid {proc.pid})")

    if wait:
        if not wait_for_boot(session, timeout=90):
            tail = session.log_text()[-6000:]
            stop_qemu(session)
            raise RuntimeError(
                f"guest did not reach supervisor loop; serial log: {session.ser_log}\n{tail}"
            )
        if not wait_for_shell(session):
            tail = session.log_text()[-6000:]
            stop_qemu(session)
            raise RuntimeError(
                f"NXShell prompt did not become ready; serial log: {session.ser_log}\n{tail}"
            )


def stop_qemu(session: Session) -> None:
    pid = session.pid()
    requested_quit = False
    if session.qmp_sock.exists():
        try:
            sock = qmp_connect(session.qmp_sock, timeout=2.0)
            try:
                qmp_send(sock, {"execute": "quit"})
                requested_quit = True
            finally:
                qmp_close(sock)
        except (OSError, RuntimeError, ValueError, ConnectionError, json.JSONDecodeError):
            pass

    deadline = time.time() + 5.0
    while time.time() < deadline:
        if pid and not session.process_matches(pid):
            break
        if not pid and not session.qmp_sock.exists():
            break
        time.sleep(0.15)

    # Only signal the exact process recorded by this session, and only after
    # verifying its command line contains this session's unique QMP socket.
    if pid and session.process_matches(pid):
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        deadline = time.time() + 2.0
        while time.time() < deadline and session.process_matches(pid):
            time.sleep(0.1)
        if session.process_matches(pid):
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    elif not pid and session.qmp_sock.exists() and not requested_quit:
        print("[nxadb] QMP socket is stale/unresponsive; no owned PID to signal", file=sys.stderr)

    session.pid_file.unlink(missing_ok=True)
    # If QEMU has exited, the socket file can safely be removed.
    if not session.is_running():
        session.qmp_sock.unlink(missing_ok=True)
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
    """Wait for the actual interactive prompt, not merely the early init log."""
    print("[nxadb] waiting for NXShell prompt...", end="", flush=True)
    started = False
    t0 = time.time()
    while time.time() - t0 < timeout:
        text = strip_ansi(session.log_text())
        if SHELL_ALIVE_RE.search(text):
            started = True
        if PROMPT_RE.search(text):
            print(" ready")
            return True
        if FAULT_RE.search(text):
            print(" FAULT")
            return False
        time.sleep(0.35)
        print(".", end="", flush=True)
    print(" TIMEOUT" if started else " NOT-STARTED")
    return False


# ---------------------------------------------------------------------------
# Robust output capture (prompt-based)
# ---------------------------------------------------------------------------

def wait_for_prompt(session: Session, after_size: int, timeout: float = 25.0) -> str:
    """
    Wait for a prompt to reappear, returning all output since after_size.
    A short settle interval avoids returning halfway through prompt rendering;
    unlike whole-log stability, it cannot hang on periodic guest diagnostics.
    Kernel faults are returned immediately so callers can fail fast.
    """
    deadline = time.monotonic() + timeout
    prompt_since = None
    new = ""

    while time.monotonic() < deadline:
        text = session.log_text()
        new = text[after_size:] if after_size <= len(text) else text
        clean = strip_ansi(new)

        if FAULT_RE.search(clean):
            return new

        now = time.monotonic()
        if PROMPT_RE.search(clean):
            if prompt_since is None:
                prompt_since = now
            elif now - prompt_since >= 0.4:
                return new
        else:
            prompt_since = None

        time.sleep(0.1)

    text = session.log_text()
    return text[after_size:] if after_size <= len(text) else text


def run_shell_c(session: Session, command: str, timeout: float = 30.0) -> Tuple[int, str]:
    """
    Execute directly at the already-running NXShell prompt.
    Avoids spawning a second /bin/nxshell process, which is not installed in
    the current rootfs image.
    """
    if not session.is_running():
        start_qemu(session)

    before = session.log_size()
    typed = _to_ascii(command) + "\n"
    qmp_type(session.qmp_sock, typed)

    new = wait_for_prompt(session, before, timeout=timeout)

    code = 0 if PROMPT_RE.search(strip_ansi(new)) else 124
    lower = new.lower()
    if "unknown command:" in lower or "command not found" in lower or "file not found:" in lower:
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
            clean = strip_ansi(l).strip()
            if PROMPT_RE.search(strip_ansi(l)) or "nxshell -c" in l or clean == command.strip():
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
                if PROMPT_RE.search(strip_ansi(l)):
                    continue
                print(l)
    except KeyboardInterrupt:
        print()
    return 0


def cmd_ls(session: Session, path: str = ".") -> int:
    command = f"ls {path}"
    code, out = run_shell_c(session, command)
    for l in out.splitlines():
        if (not PROMPT_RE.search(strip_ansi(l)) and "nxshell -c" not in l
                and strip_ansi(l).strip() != command):
            print(l)
    return code


def cmd_cat(session: Session, path: str) -> int:
    command = f"cat {path}"
    code, out = run_shell_c(session, command)
    for l in out.splitlines():
        if (not PROMPT_RE.search(strip_ansi(l)) and "nxshell -c" not in l
                and strip_ansi(l).strip() != command):
            print(l)
    return code


def cmd_pull(session: Session, remote: str, local: str) -> int:
    """pull = cat <remote> with cleaning"""
    print(f"[nxadb] pull {remote} → {local}")
    code, out = run_shell_c(session, f"cat {remote}", timeout=40)

    lines = []
    for l in out.splitlines():
        if PROMPT_RE.search(strip_ansi(l)):
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
    running = session.is_running()
    print(f"arch     : {session.arch}")
    print(f"running  : {running}")
    if running:
        print(f"pid      : {session.pid()}")
        text = session.log_text()
        print(f"shell    : {'yes' if SHELL_ALIVE_RE.search(strip_ansi(text)) else 'no'}")
        print(f"faults   : {len(FAULT_RE.findall(text))}")
        print(f"log size : {session.log_size()} characters")


def cmd_test(session: Session, cmds: List[str], expect: List[str], runs: int) -> int:
    if runs < 1:
        print("[nxadb] --runs must be >= 1", file=sys.stderr)
        return 2

    rc = 0
    ktest_re = re.compile(
        r"\[KTEST\] Completed\. Summary:\s*(\d+)\s+PASSED,\s*(\d+)\s+FAILED",
        re.IGNORECASE,
    )

    for run_index in range(1, runs + 1):
        print(f"\n=== run {run_index}/{runs} ({session.arch}) ===")
        stop_qemu(session)
        start_qemu(session)

        verdict = "ok"
        results: List[str] = []
        text = session.log_text()
        ktest = ktest_re.search(text)
        if not ktest:
            results.append("KTEST=NO-RESULT")
            verdict = "FAIL"
        else:
            passed, failed = map(int, ktest.groups())
            results.append(f"KTEST={passed} passed/{failed} failed")
            if passed == 0 or failed != 0:
                verdict = "FAIL"

        for command in cmds:
            name = command.split()[0]
            before_size = session.log_size()
            code, output = run_shell_c(session, command, timeout=180)
            if code != 0:
                results.append(f"{name}=COMMAND-ERROR({code})")
                if verdict != "FAULT":
                    verdict = "FAIL"

            if not name.endswith("test"):
                continue

            done_re = re.compile(
                rf"{re.escape(name)}\]\s*done:\s*(\d+)\s*/\s*(\d+)",
                re.IGNORECASE,
            )
            matches = []
            deadline = time.time() + 140
            while time.time() < deadline:
                current = session.log_text()
                # QEMU restarts truncate the serial log. If that happened
                # after before_size was captured, scan the new log from its
                # beginning instead of waiting forever on an invalid offset.
                new_text = current[before_size:] if before_size <= len(current) else current
                matches = list(done_re.finditer(new_text))
                if matches:
                    break
                if FAULT_RE.search(new_text):
                    break
                time.sleep(0.5)

            if not matches:
                results.append(f"{name}=NO-NEW-RESULT")
                if verdict != "FAULT":
                    verdict = "FAIL"
                continue

            passed, total = map(int, matches[-1].groups())
            results.append(f"{name}={passed}/{total}")
            if total == 0 or passed != total:
                if verdict != "FAULT":
                    verdict = "FAIL"

            for expected in expect:
                if expected.startswith(f"{name}="):
                    try:
                        expected_total = int(expected.split("=", 1)[1])
                    except ValueError:
                        results.append(f"{name}=INVALID-EXPECTATION({expected})")
                        if verdict != "FAULT":
                            verdict = "FAIL"
                        continue
                    if total != expected_total:
                        results[-1] += f"(expected {expected_total})"
                        if verdict != "FAULT":
                            verdict = "FAIL"

        final_text = session.log_text()
        faults = len(FAULT_RE.findall(final_text))
        if faults:
            verdict = "FAULT"

        print(f"run{run_index}/{session.arch}: {verdict} {' '.join(results)} faults={faults}")
        if verdict != "ok":
            rc = 1
            print(f"  serial log → {session.ser_log}")
            print(f"  QEMU stderr → {session.qemu_err}")

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
    lock_file = None
    lock_acquired = False

    try:
        # All commands that can start/stop QEMU or send guest input share one
        # per-architecture lock. This prevents concurrent nxadb/nxrun callers
        # from racing over the same QMP socket, serial log and guest prompt.
        if args.cmd in MUTATING_COMMANDS:
            lock_file = (session.tmp / "session.lock").open("a+")
            deadline = time.monotonic() + SESSION_LOCK_TIMEOUT
            while True:
                try:
                    fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                    lock_acquired = True
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        raise SessionBusyError(
                            f"another {args.arch} nxadb/nxrun operation holds "
                            f"{lock_file.name}; waited {SESSION_LOCK_TIMEOUT:.0f}s"
                        )
                    time.sleep(0.1)

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
    except SessionBusyError as e:
        print(f"[nxadb] busy: {e}", file=sys.stderr)
        return 75
    except Exception as e:
        print(f"[nxadb] error: {e}", file=sys.stderr)
        return 1
    finally:
        if lock_file is not None:
            if lock_acquired:
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
            lock_file.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())