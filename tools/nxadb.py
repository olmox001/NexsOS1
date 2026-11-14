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
DEFAULT_ARCH = os.environ.get("ARCH", "aarch64")
QMP_INPUT_DISPLAY = "nxgpu0"
SESSION_LOCK_TIMEOUT = 60.0
COUNTER_FRAME_SLEEP_MS = 100
COUNTER_HEARTBEAT_FRAMES = 50
COUNTER_HEARTBEAT_BUDGET_MULTIPLIER = 3
COUNTER_HEARTBEAT_TIMEOUT = (
    COUNTER_FRAME_SLEEP_MS * COUNTER_HEARTBEAT_FRAMES *
    COUNTER_HEARTBEAT_BUDGET_MULTIPLIER / 1000.0
)
SHELL_PROMPT_SETTLE_SECONDS = 0.75
BOOT_NXTEST_DISCOVERY_SECONDS = 5.0
BOOT_NXTEST_TIMEOUT_SECONDS = 180.0
BOOT_NXTEST_DISMISS_SETTLE_SECONDS = 0.50
MUTATING_COMMANDS = {"start", "stop", "restart", "shell", "ls", "cat",
                     "push", "pull", "test", "screenshot", "mouse"}


class SessionBusyError(RuntimeError):
    """Another nxadb/nxrun process owns this architecture's guest session."""


FAULT_RE = re.compile(
    r"KERNEL PANIC|KERNEL PAGE FAULT|GENERAL PROTECTION FAULT|"
    r"Unrecoverable kernel|Invalid magic|"
    r"\[FATAL\]\s*NESTED\s+(?:EL1|SError|AArch32|EXCEPTION)|"
    r"\bNESTED CPU EXCEPTION\b|"
    r"\bStack Smash at\b|"
    r"\bSystem halted\b",
    re.IGNORECASE,
)
ANSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
SHELL_ALIVE_RE = re.compile(
    r"NXShell: Alive|NXShell PID|NXShell started \(PID|NXShell\] TTY Window .* active",
    re.IGNORECASE,
)
PROMPT_RE = re.compile(r"NXShell:[^\r\n]*>\s*", re.IGNORECASE)


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def _nxwins_focused_tty_window(text: str) -> Optional[str]:
    """Return the focused TTY host window from nxwins structured output.

    The graphical terminal is owned by nxttyd; nxshell is the hosted job
    inside that terminal and therefore must not be used as the WINDOW owner
    identity. Requiring the canonical service path catches regressions back to
    the pre-execsvc/TTY ownership model instead of accepting either spelling.
    """
    clean = strip_ansi(text)
    if not re.search(r"(?mi)^\s*ID\s+PID\s+STATE\b", clean):
        return None
    match = re.search(
        r"(?mi)^\s*(\d+)\s+\d+\s+focus\s+.*\s/sys/services/nxttyd\s*$",
        clean,
    )
    return match.group(1) if match else None

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


def qmp_type(sock_path: Path, text: str, delay: float = 0.075) -> None:
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


def qmp_mouse_rel(sock_path: Path, dx: int, dy: int,
                  button: Optional[str] = None, delay: float = 0.12) -> None:
    """Send relative pointer motion and an optional edge-separated button click."""
    if button not in (None, "left", "middle", "right"):
        raise ValueError("mouse button must be left, middle, right or None")
    if not 0.01 <= delay <= 1.0:
        raise ValueError("mouse click delay must be between 0.01 and 1.0 seconds")
    sock = qmp_connect(sock_path)
    try:
        events = []
        if dx:
            events.append({"type": "rel", "data": {"axis": "x", "value": int(dx)}})
        if dy:
            events.append({"type": "rel", "data": {"axis": "y", "value": int(dy)}})
        if events:
            response = qmp_send(sock, {
                "execute": "input-send-event",
                "arguments": {"device": QMP_INPUT_DISPLAY, "head": 0, "events": events},
            })
            if "error" in response:
                raise RuntimeError(f"QMP pointer motion failed: {response['error']}")
        if button:
            for down in (True, False):
                response = qmp_send(sock, {
                    "execute": "input-send-event",
                    "arguments": {
                        "device": QMP_INPUT_DISPLAY,
                        "head": 0,
                        "events": [{"type": "btn", "data": {"button": button, "down": down}}],
                    },
                })
                if "error" in response:
                    raise RuntimeError(f"QMP mouse button failed: {response['error']}")
                time.sleep(delay)
    finally:
        qmp_close(sock)


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
    build_root: Path
    boot_mode: str
    firmware: str
    tmp: Path
    ser_log: Path
    qmp_sock: Path
    qemu_err: Path
    qemu_out: Path
    pid_file: Path
    vars_file: Path
    qemu_bin: str = ""
    display: str = "none"

    @classmethod
    def create(
        cls, arch: str, root: Path, boot_mode: str = "iso",
        build_root: Path = Path("build"), firmware: str = "auto",
    ) -> "Session":
        root = root.resolve()
        boot_mode = boot_mode.lower()
        firmware = firmware.lower()
        if boot_mode not in ("iso", "direct"):
            raise ValueError(f"unsupported boot mode: {boot_mode}")
        if firmware not in ("auto", "bios", "uefi"):
            raise ValueError(f"unsupported firmware mode: {firmware}")
        if boot_mode == "direct":
            if firmware != "auto":
                raise ValueError("firmware selection applies only to ISO boot mode")
        elif arch == "aarch64":
            if firmware == "bios":
                raise ValueError("AArch64 ISO boot requires UEFI firmware")
            firmware = "uefi"
        elif firmware == "auto":
            firmware = "bios"
        resolved_build = (
            build_root if build_root.is_absolute() else root / build_root
        ).resolve()
        try:
            relative_build = resolved_build.relative_to(root)
        except ValueError as exc:
            raise ValueError("--build-root must stay inside the project") from exc
        if relative_build == Path("."):
            raise ValueError("--build-root cannot be the project root")

        state_key = boot_mode if boot_mode == "direct" else f"{boot_mode}-{firmware}"
        state_dir = resolved_build / "runtime" / "nxadb" / arch / state_key
        state_dir.mkdir(parents=True, exist_ok=True)
        qmp_dir = resolved_build / "q"
        qmp_dir.mkdir(parents=True, exist_ok=True)
        return cls(
            arch=arch,
            root=root,
            build_root=resolved_build,
            boot_mode=boot_mode,
            firmware=firmware,
            tmp=state_dir,
            ser_log=state_dir / "serial.log",
            qmp_sock=qmp_dir / f"{arch}-{state_key}.sock",
            qemu_err=state_dir / "qemu.stderr.log",
            qemu_out=state_dir / "qemu.stdout.log",
            pid_file=state_dir / "qemu.pid.json",
            vars_file=state_dir / "uefi-vars.fd",
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
        return bool(
            command and str(self.qmp_sock) in command and "qemu-system-" in command
        )

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
            except (
                OSError, RuntimeError, ValueError, ConnectionError,
                json.JSONDecodeError,
            ):
                pass
        return self.process_matches(self.pid())

    def build_dir(self) -> Path:
        return self.build_root / self.arch

    def boot_iso(self) -> Path:
        if self.arch == "amd64":
            if self.firmware == "uefi":
                return self.build_dir() / "grub-uefi" / "nexsos-amd64-uefi.iso"
            return self.build_dir() / "grub-bios.iso"
        return self.build_dir() / "grub-efi" / "nexsos-aarch64-uefi.iso"

    def lane_name(self) -> str:
        if self.boot_mode == "direct":
            return "direct"
        return f"iso-{self.firmware}"

    def kernel_image(self) -> Path:
        name = "kernel.elf" if self.arch == "amd64" else "kernel.bin"
        return self.build_dir() / name

    def dtb_image(self) -> Optional[Path]:
        if self.arch != "aarch64":
            return None
        return self.build_dir() / "virt.dtb"

    def disk_image(self) -> Path:
        return self.build_dir() / "disk.img"

    def make_build_root(self) -> str:
        return str(self.build_root.relative_to(self.root))

    def required_artifacts(self) -> List[Path]:
        artifacts = [self.disk_image(), self.kernel_image()]
        if self.boot_mode == "iso":
            artifacts.append(self.boot_iso())
        elif self.arch == "aarch64":
            dtb = self.dtb_image()
            if dtb is not None:
                artifacts.append(dtb)
        return artifacts

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
        session.boot_iso() if session.boot_mode == "iso" else None,
        session.disk_image(),
        serial=f"file:{session.ser_log}",
        display=session.display,
        qmp_socket=session.qmp_sock,
        uefi_vars_file=session.vars_file,
        boot_mode=session.boot_mode,
        kernel=session.kernel_image(),
        dtb=session.dtb_image(),
        firmware=session.firmware,
    )


def ensure_artifacts(session: Session) -> None:
    required = session.required_artifacts()
    if session.boot_mode == "direct":
        target = "direct-artifacts"
    elif session.arch == "amd64" and session.firmware == "uefi":
        target = "boot-uefi"
    else:
        target = "boot-iso"
    print(
        f"[nxadb] verifying build ARCH={session.arch} "
        f"lane={session.lane_name()} target={target}"
    )
    run_dir = session.build_root / "runs" / session.arch / session.lane_name()
    run_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    build_log = (
        run_dir / f"nxadb-build-{stamp}-{time.time_ns() % 1000000000:09d}.log"
    )
    command = [
        "make", f"BUILD_ROOT={session.make_build_root()}",
        f"ARCH={session.arch}", target,
    ]
    with build_log.open("w") as output:
        result = subprocess.run(
            command, cwd=session.root, stdout=output, stderr=subprocess.STDOUT,
        )
    print(f"[nxadb] build transcript: {build_log}")
    if result.returncode != 0:
        raise RuntimeError(
            f"{' '.join(command)} failed ({result.returncode}); "
            f"complete build transcript: {build_log}"
        )
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise RuntimeError(
            f"{target} completed without required artifacts: {', '.join(missing)}"
        )


def start_qemu(session: Session, wait: bool = True) -> None:
    if session.is_running():
        print(f"[nxadb] already running (pid {session.pid() or 'QMP-owned'})")
        return

    sibling_specs = (
        ("direct", "auto"),
        ("iso", "bios"),
        ("iso", "uefi"),
    )
    for sibling_mode, sibling_firmware in sibling_specs:
        try:
            sibling = Session.create(
                session.arch, session.root, sibling_mode,
                session.build_root.relative_to(session.root),
                firmware=sibling_firmware,
            )
        except ValueError:
            continue
        if sibling.tmp == session.tmp or not sibling.is_running():
            continue
        print(
            f"[nxadb] stopping sibling {session.arch}/{sibling.lane_name()} "
            "guest that owns the shared rootfs image"
        )
        stop_qemu(sibling)

    ensure_artifacts(session)
    session.tmp.mkdir(parents=True, exist_ok=True)
    session.qmp_sock.parent.mkdir(parents=True, exist_ok=True)
    # Preserve every previous capture before QEMU opens its file-backed UART.
    # A failed boot must never erase the only copy of an earlier firmware log.
    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    archive = (
        session.build_root / "runs" / session.arch / session.lane_name() /
        f"boot-{stamp}-{time.time_ns() % 1000000000:09d}"
    )
    archive.mkdir(parents=True, exist_ok=False)
    for source in (session.ser_log, session.qemu_err, session.qemu_out):
        if source.exists():
            source.replace(archive / source.name)
    for path in (session.qmp_sock, session.pid_file):
        path.unlink(missing_ok=True)

    cmd = build_qemu_cmdline(session)
    session.qemu_bin = cmd[0]
    env = clean_qemu_environment()
    with session.qemu_err.open("w") as err, session.qemu_out.open("w") as out:
        proc = subprocess.Popen(
            cmd, cwd=session.root, stdout=out, stderr=err,
            env=env, start_new_session=True,
        )
    run_manifest = {
        "pid": proc.pid,
        "arch": session.arch,
        "boot_mode": session.boot_mode,
        "firmware": session.firmware,
        "lane": session.lane_name(),
        "build_root": str(session.build_root.relative_to(session.root)),
        "qmp_socket": str(session.qmp_sock),
        "qemu": session.qemu_bin,
        "display": session.display,
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "command": cmd,
        "logs": {
            "serial": str(session.ser_log.relative_to(session.root)),
            "qemu_stdout": str(session.qemu_out.relative_to(session.root)),
            "qemu_stderr": str(session.qemu_err.relative_to(session.root)),
        },
        "previous_logs_archive": str(archive.relative_to(session.root)),
    }
    session.pid_file.write_text(json.dumps(run_manifest, indent=2) + "\n")
    manifest_path = archive.parent / f"run-{stamp}-{time.time_ns() % 1000000000:09d}.json"
    manifest_path.write_text(json.dumps(run_manifest, indent=2) + "\n")
    print(
        f"[nxadb] QEMU started ({session.arch}, lane={session.lane_name()}, "
        f"pid {proc.pid})"
    )
    print(f"[nxadb] complete UART: {session.ser_log}")
    print(f"[nxadb] QEMU stdout/stderr: {session.qemu_out} | {session.qemu_err}")
    print(f"[nxadb] previous run archived: {archive}")

    if wait:
        if not wait_for_boot(session, timeout=90):
            tail = session.log_text()[-6000:]
            qemu_err = session.qemu_err.read_text(errors="replace")[-6000:] if session.qemu_err.exists() else ""
            qemu_out = session.qemu_out.read_text(errors="replace")[-6000:] if session.qemu_out.exists() else ""
            stop_qemu(session)
            raise RuntimeError(
                "guest did not reach supervisor loop; captured output is NOT truncated on disk.\n"
                f"UART: {session.ser_log}\nQEMU stdout: {session.qemu_out}\n"
                f"QEMU stderr: {session.qemu_err}\nPrevious archive: {archive}\n"
                f"--- last 6000 characters of UART ---\n{tail}\n"
                f"--- last 6000 characters of QEMU stderr ---\n{qemu_err}\n"
                f"--- last 6000 characters of QEMU stdout ---\n{qemu_out}"
            )
        if not wait_for_shell(session):
            tail = session.log_text()[-6000:]
            qemu_err = session.qemu_err.read_text(errors="replace")[-6000:] if session.qemu_err.exists() else ""
            qemu_out = session.qemu_out.read_text(errors="replace")[-6000:] if session.qemu_out.exists() else ""
            stop_qemu(session)
            raise RuntimeError(
                "NXShell prompt did not become ready; captured output is NOT truncated on disk.\n"
                f"UART: {session.ser_log}\nQEMU stdout: {session.qemu_out}\n"
                f"QEMU stderr: {session.qemu_err}\nPrevious archive: {archive}\n"
                f"--- last 6000 characters of UART ---\n{tail}\n"
                f"--- last 6000 characters of QEMU stderr ---\n{qemu_err}\n"
                f"--- last 6000 characters of QEMU stdout ---\n{qemu_out}"
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


def boot_has_live_userland(text: str) -> bool:
    """Boot acceptance needs evidence of a real running shell, not a K3 log.

    Accept the prior supervisor marker for older images, but recognize the
    new kernel-agnostic initial-task handoff plus a live userland shell.
    wait_for_shell() separately verifies the actual interactive prompt.
    """
    if "Entering supervisor loop" in text:
        return True
    return bool(re.search(r"K3: Initialized PID 1\b", text) and
                SHELL_ALIVE_RE.search(strip_ansi(text)))


def wait_for_boot(session: Session, timeout: int = 90) -> bool:
    print("[nxadb] waiting for kernel...", end="", flush=True)
    t0 = time.time()
    while time.time() - t0 < timeout:
        text = session.log_text()
        if boot_has_live_userland(text):
            print(" ok")
            return True
        if FAULT_RE.search(text):
            print(" FAULT")
            return False
        # QEMU can exit immediately when another run owns the writable disk
        # image. Do not burn the full boot timeout polling a dead guest.
        if time.time() - t0 >= 1.5 and not session.process_matches(session.pid()):
            print(" QEMU-EXITED")
            return False
        time.sleep(0.3)
        print(".", end="", flush=True)
    print(" TIMEOUT")
    return False


def _prompt_settle_state(
    text: str, prompt_since: Optional[float], now: float
) -> Tuple[bool, Optional[float]]:
    """Require a continuously visible prompt before injecting QMP input."""
    if not PROMPT_RE.search(text):
        return False, None
    if prompt_since is None:
        return False, now
    return now - prompt_since >= SHELL_PROMPT_SETTLE_SECONDS, prompt_since


def wait_for_shell(session: Session, timeout: int = 50) -> bool:
    """Wait for a stable interactive prompt, not merely the early init log."""
    print("[nxadb] waiting for NXShell prompt...", end="", flush=True)
    started = False
    prompt_since: Optional[float] = None
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        text = strip_ansi(session.log_text())
        if SHELL_ALIVE_RE.search(text):
            started = True
        ready, prompt_since = _prompt_settle_state(
            text, prompt_since, time.monotonic()
        )
        if ready:
            print(" ready")
            return True
        if FAULT_RE.search(text):
            print(" FAULT")
            return False
        if time.monotonic() - t0 >= 1.5 and not session.process_matches(session.pid()):
            print(" QEMU-EXITED")
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


def cmd_mouse(session: Session, dx: int, dy: int, click: bool = False,
              button: str = "left", delay: float = 0.12) -> int:
    if not session.is_running():
        raise RuntimeError(f"{session.arch} guest is not running")
    qmp_mouse_rel(session.qmp_sock, dx, dy, button if click else None, delay)
    print(f"[nxadb] pointer moved dx={dx} dy={dy}" + (f"; clicked {button}" if click else ""))
    return 0


def cmd_screenshot(session: Session, requested_path: Optional[str] = None) -> int:
    """Capture the active QEMU display through QMP and verify the PNG."""
    if not session.is_running():
        raise RuntimeError(f"{session.arch} guest is not running")

    run_dir = (
        session.build_root / "runs" / session.arch / session.lane_name()
    ).resolve()
    if requested_path:
        output = Path(requested_path).expanduser()
        if not output.is_absolute():
            output = run_dir / output
        output = output.resolve()
        if not output.is_relative_to(run_dir):
            raise ValueError(f"screenshot path must stay under {run_dir}")
        if output.suffix.lower() != ".png":
            raise ValueError("screenshot path must end in .png")
    else:
        stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
        output = run_dir / f"{session.arch}-{stamp}-{time.time_ns() % 1000000000:09d}.png"

    ppm = output.with_suffix(".ppm")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists() or ppm.exists():
        raise FileExistsError(f"refusing to overwrite screenshot: {output}")

    sock = qmp_connect(session.qmp_sock)
    try:
        response = qmp_send(sock, {
            "execute": "screendump",
            "arguments": {
                "filename": str(ppm),
                "device": QMP_INPUT_DISPLAY,
                "head": 0,
            },
        })
        if "error" in response:
            raise RuntimeError(f"QMP screendump failed: {response['error']}")
    finally:
        qmp_close(sock)

    if not ppm.is_file() or ppm.stat().st_size <= 16:
        raise RuntimeError(f"QEMU did not produce a valid PPM screenshot: {ppm}")
    converted = subprocess.run(
        ["sips", "-s", "format", "png", str(ppm), "--out", str(output)],
        capture_output=True, text=True,
    )
    if converted.returncode != 0 or not output.is_file():
        raise RuntimeError(f"PPM→PNG conversion failed: {converted.stderr.strip()}")
    with output.open("rb") as image_file:
        if image_file.read(8) != b"\x89PNG\r\n\x1a\n":
            output.unlink(missing_ok=True)
            raise RuntimeError("sips output is not a valid PNG")

    metadata = subprocess.run(
        ["sips", "-g", "pixelWidth", "-g", "pixelHeight", str(output)],
        capture_output=True, text=True, check=True,
    ).stdout
    try:
        saved_session = json.loads(session.pid_file.read_text())
        session.display = saved_session.get("display", session.display)
    except (OSError, ValueError, AttributeError):
        pass
    manifest = {
        "arch": session.arch,
        "pid": session.pid(),
        "display": session.display,
        "captured_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "image": str(output.relative_to(session.root)),
        "raw_ppm": str(ppm.relative_to(session.root)),
        "serial_log": str(session.ser_log.relative_to(session.root)),
        "dimensions": metadata.strip(),
    }
    output.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"[nxadb] screenshot verified: {output}")
    print(metadata.strip())
    return 0


def cmd_bootlog(session: Session, history: bool = False) -> None:
    """Print complete captured channels; never tail/truncate the stored logs."""
    channels = (
        ("GUEST UART / UEFI SERIAL", session.ser_log),
        ("QEMU STDOUT", session.qemu_out),
        ("QEMU STDERR", session.qemu_err),
    )
    for label, log_path in channels:
        print(f"\n===== {label}: {log_path} =====")
        if log_path.is_file():
            content = log_path.read_text(errors="replace")
            sys.stdout.write(content)
            if content and not content.endswith("\n"):
                print()
        else:
            print("[not captured]")
    run_dir = session.build_root / "runs" / session.arch / session.lane_name()
    manifests = sorted(run_dir.glob("run-*.json"))
    if manifests:
        selected = manifests if history else manifests[-1:]
        for manifest in selected:
            print(f"\n===== RUN MANIFEST: {manifest} =====")
            sys.stdout.write(manifest.read_text(errors="replace"))
    if history:
        for archive in sorted(run_dir.glob("boot-*")):
            if archive.is_dir():
                print(f"[archived prior logs] {archive}")


def cmd_status(session: Session) -> None:
    running = session.is_running()
    print(f"arch     : {session.arch}")
    print(f"mode     : {session.boot_mode}")
    print(f"firmware : {session.firmware}")
    print(f"lane     : {session.lane_name()}")
    print(f"build    : {session.build_root.relative_to(session.root)}")
    print(f"running  : {running}")
    print(f"UART     : {session.ser_log}")
    print(f"QEMU out : {session.qemu_out}")
    print(f"QEMU err : {session.qemu_err}")
    if running:
        print(f"pid      : {session.pid()}")
        text = session.log_text()
        print(f"shell    : {'yes' if SHELL_ALIVE_RE.search(strip_ansi(text)) else 'no'}")
        print(f"faults   : {len(FAULT_RE.findall(text))}")
        print(f"log size : {session.log_size()} characters")


def _shell_health_probe(session: Session) -> Tuple[bool, str]:
    code, output = run_shell_c(session, "pwd", timeout=20)
    clean_lines = [
        strip_ansi(line).strip() for line in output.splitlines()
        if strip_ansi(line).strip()
    ]
    healthy = code == 0 and any(line == "/home" for line in clean_lines)
    return healthy, output


def _shell_navigation_probe(session: Session) -> Tuple[bool, str]:
    marker = "NXADB_NAV_UP_HISTORY"
    code, output = run_shell_c(session, f"echo {marker}", timeout=20)
    if code != 0:
        return False, output

    initial_lines = [
        strip_ansi(line).strip() for line in output.replace("\r", "").splitlines()
    ]
    if marker not in initial_lines:
        return False, output

    before = session.log_size()
    qmp_special(session.qmp_sock, "up")
    qmp_special(session.qmp_sock, "ret")
    repeated = wait_for_prompt(session, before, timeout=10)
    repeated_lines = [
        strip_ansi(line).strip()
        for line in repeated.replace("\r", "").splitlines()
    ]
    return marker in repeated_lines, repeated


def _boot_nxtest_state(text: str) -> str:
    clean = strip_ansi(text)
    if re.search(r"\[NXTEST\]\s+done:", clean, re.IGNORECASE):
        return "done"
    if re.search(
        r"\[NXTEST\]\s+real userland suite started", clean, re.IGNORECASE
    ):
        return "running"
    return "absent"


def _dismiss_boot_nxtest_tty(session: Session) -> None:
    """Close the completed boot-test TTY through its normal Enter lifecycle."""
    if not session.is_running():
        return
    qmp_special(session.qmp_sock, "ret")
    time.sleep(BOOT_NXTEST_DISMISS_SETTLE_SECONDS)


def _wait_for_boot_nxtest(session: Session) -> Tuple[bool, str]:
    discovery_deadline = time.monotonic() + BOOT_NXTEST_DISCOVERY_SECONDS
    state = _boot_nxtest_state(session.log_text())
    while state == "absent" and time.monotonic() < discovery_deadline:
        live = session.log_text()
        if FAULT_RE.search(live) or not session.is_running():
            return False, "fault"
        time.sleep(0.10)
        state = _boot_nxtest_state(session.log_text())
    if state == "absent":
        return True, state
    if state == "done":
        _dismiss_boot_nxtest_tty(session)
        return True, state

    deadline = time.monotonic() + BOOT_NXTEST_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        live = session.log_text()
        if FAULT_RE.search(live) or not session.is_running():
            return False, "fault"
        state = _boot_nxtest_state(live)
        if state == "done":
            _dismiss_boot_nxtest_tty(session)
            return True, state
        time.sleep(0.20)
    return False, "timeout"


def _run_timerdiag(session: Session) -> Tuple[bool, str]:
    code, output = run_shell_c(session, "/sys/bin/nxtimerdiag --probe", timeout=45)
    clean = strip_ansi(output)
    done = re.search(
        r"\[TIMERDIAG\]\s+DONE\b[^\r\n]*failures=(\d+)",
        clean, re.IGNORECASE,
    )
    return code == 0 and done is not None and int(done.group(1)) == 0, output


def _counter_heartbeat_count(text: str) -> int:
    clean = strip_ansi(text)
    current = re.findall(
        r"(?m)^\s*\[COUNTER\]\s+"
        r"frame=(?:0x[0-9a-fA-F]+|[0-9]+)\s+"
        r"mono_ms=(?:0x[0-9a-fA-F]+|[0-9]+)"
        r"(?:\s+delta_ms=[0-9]+)?\s*$",
        clean,
    )
    legacy = re.findall(r"(?m)^\s*Count:\s*0x[0-9a-fA-F]+\s*$", clean)
    return len(current) + len(legacy)


def _run_counter_heartbeat(session: Session) -> Tuple[bool, float]:
    before = _counter_heartbeat_count(session.log_text())
    code, _ = run_shell_c(session, "/bin/counter", timeout=20)
    if code != 0:
        return False, 0.0

    started = time.monotonic()
    deadline = started + COUNTER_HEARTBEAT_TIMEOUT
    while time.monotonic() < deadline:
        live = session.log_text()
        if FAULT_RE.search(live) or not session.is_running():
            return False, time.monotonic() - started
        if _counter_heartbeat_count(live) > before:
            return True, time.monotonic() - started
        time.sleep(0.25)
    return False, time.monotonic() - started


def cmd_test(session: Session, cmds: List[str], expect: List[str], runs: int,
             soak_seconds: int = 0) -> int:
    if runs < 1:
        print("[nxadb] --runs must be >= 1", file=sys.stderr)
        return 2

    rc = 0
    ktest_re = re.compile(
        r"\[KTEST\] Completed\. Summary:\s*(\d+)\s+PASSED,\s*(\d+)\s+FAILED",
        re.IGNORECASE,
    )

    for run_index in range(1, runs + 1):
        print(
            f"\n=== run {run_index}/{runs} "
            f"({session.arch}/{session.lane_name()}) ==="
        )
        stop_qemu(session)
        start_qemu(session)

        verdict = "ok"
        results: List[str] = []
        boot_tests_ok, boot_tests_state = _wait_for_boot_nxtest(session)
        results.append(f"BOOT-NXTEST={boot_tests_state.upper()}")
        if not boot_tests_ok:
            verdict = "FAULT"

        if verdict != "FAULT":
            healthy, _ = _shell_health_probe(session)
            if healthy:
                results.append("SHELL-INTERACTION=PASS")
            else:
                results.append("SHELL-INTERACTION=FAIL")
                verdict = "FAULT"

        if verdict != "FAULT":
            navigation_ok, _ = _shell_navigation_probe(session)
            if navigation_ok:
                results.append("SHELL-UP-HISTORY=PASS")
            else:
                results.append("SHELL-UP-HISTORY=FAIL")
                verdict = "FAULT"

        if verdict != "FAULT":
            timer_ok, _ = _run_timerdiag(session)
            if timer_ok:
                results.append("TIMERDIAG=PASS")
            else:
                results.append("TIMERDIAG=FAIL")
                verdict = "FAULT"
            post_timer_ok, _ = _shell_health_probe(session)
            if post_timer_ok:
                results.append("SHELL-AFTER-TIMER=PASS")
            else:
                results.append("SHELL-AFTER-TIMER=FAIL")
                verdict = "FAULT"

        if verdict != "FAULT":
            heartbeat_ok, heartbeat_seconds = _run_counter_heartbeat(session)
            if heartbeat_ok:
                results.append(
                    f"COUNTER-HEARTBEAT=PASS({heartbeat_seconds:.1f}s)"
                )
            else:
                results.append(
                    f"COUNTER-HEARTBEAT=FAIL({heartbeat_seconds:.1f}s)"
                )
                verdict = "FAULT"
            post_heartbeat_ok, _ = _shell_health_probe(session)
            if post_heartbeat_ok:
                results.append("SHELL-AFTER-HEARTBEAT=PASS")
            else:
                results.append("SHELL-AFTER-HEARTBEAT=FAIL")
                verdict = "FAULT"

        if soak_seconds:
            # Capture a baseline screen, exercise the userland window listing,
            # then supervise the live guest for the requested hardening window.
            try:
                cmd_screenshot(session)
            except Exception as exc:
                results.append(f"SCREENSHOT-START-FAIL({exc})")
                verdict = "FAIL"
            nxwins_code, nxwins_output = run_shell_c(
                session, "/sys/bin/nxwins", timeout=20
            )
            nxwins_text = strip_ansi(nxwins_output)
            window_id = _nxwins_focused_tty_window(nxwins_text)
            # The visible terminal WINDOW is owned by nxttyd. nxshell is a
            # hosted job inside that TTY and is not a WINDOW owner.
            if nxwins_code != 0 or window_id is None:
                results.append(f"NXWINS-FAIL({nxwins_code})")
                verdict = "FAIL"
            else:
                focus_code, focus_output = run_shell_c(
                    session, f"/sys/bin/nxwins focus {window_id}", timeout=20
                )
                if f"focused window {window_id}" not in strip_ansi(focus_output).lower():
                    results.append(f"NXWINS-FOCUS-FAIL({focus_code})")
                    verdict = "FAIL"
                else:
                    results.append(f"NXWINS=PASS(window={window_id},focus=PASS)")
            soak_start = time.monotonic()
            while time.monotonic() - soak_start < soak_seconds:
                if not session.is_running():
                    results.append("QEMU-EXITED-DURING-SOAK")
                    verdict = "FAULT"
                    break
                live = session.log_text()
                if FAULT_RE.search(live):
                    results.append("KERNEL-FAULT-DURING-SOAK")
                    verdict = "FAULT"
                    break
                time.sleep(1.0)
            results.append(f"SOAK={int(time.monotonic() - soak_start)}s")
            if verdict != "FAULT":
                # QEMU can remain alive while the guest kernel is halted or
                # the only shell task has died. Prove the guest still services
                # a real command after the soak; process liveness alone is not
                # a health signal.
                health_code, health_output = run_shell_c(
                    session, "/sys/bin/nxwins", timeout=20
                )
                health_text = strip_ansi(health_output)
                tty_window = _nxwins_focused_tty_window(health_text)
                if health_code != 0 or tty_window is None:
                    results.append(f"GUEST-HEALTH-FAIL({health_code})")
                    verdict = "FAULT"
                else:
                    results.append("GUEST-HEALTH=PASS")
                try:
                    cmd_screenshot(session)
                    results.append("SCREENSHOT-END=PASS")
                except Exception as exc:
                    results.append(f"SCREENSHOT-END-FAIL({exc})")
                    if verdict != "FAULT":
                        verdict = "FAIL"

        text = session.log_text()
        ktest = ktest_re.search(text)
        if not ktest:
            results.append("KTEST=NO-RESULT")
            if verdict != "FAULT":
                verdict = "FAIL"
        else:
            passed, failed = map(int, ktest.groups())
            results.append(f"KTEST={passed} passed/{failed} failed")
            if (passed == 0 or failed != 0) and verdict != "FAULT":
                verdict = "FAIL"

        for command in cmds:
            name = Path(command.split()[0]).name
            before_size = session.log_size()
            code, output = run_shell_c(session, command, timeout=180)
            if code != 0:
                results.append(f"{name}=COMMAND-ERROR({code})")
                if verdict != "FAULT":
                    verdict = "FAIL"

            # Explicit --expect entries may opt any command into the standard
            # [basename] done: passed/total result contract; commands ending
            # in "test" retain the historical automatic KTEST-style parsing.
            has_expectation = any(expected.startswith(f"{name}=") for expected in expect)
            if not name.endswith("test") and not has_expectation:
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

        print(
            f"run{run_index}/{session.arch}/{session.lane_name()}: {verdict} "
            f"{' '.join(results)} faults={faults}"
        )
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
    parser.add_argument(
        "--boot-mode", choices=["iso", "direct"], default="iso",
        help="iso=GRUB image path; direct=QEMU -kernel with the same rootfs",
    )
    parser.add_argument(
        "--firmware", choices=["auto", "bios", "uefi"], default="auto",
        help="ISO firmware: amd64 auto=bios; aarch64 auto=uefi",
    )
    parser.add_argument(
        "--build-root", type=Path, default=Path("build"),
        help="project-relative isolated build namespace (default: build)",
    )

    sub = parser.add_subparsers(dest="cmd", required=True)

    p_start = sub.add_parser("start", help="Boot the guest")
    p_start.add_argument("--display", choices=["none", "default"], default="none",
                         help="none=headless automation; default=visible QEMU window")
    sub.add_parser("stop", help="Stop the guest")
    p_restart = sub.add_parser("restart", help="Restart the guest")
    p_restart.add_argument("--display", choices=["none", "default"], default="none",
                           help="none=headless automation; default=visible QEMU window")
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

    p_log = sub.add_parser("logcat", help="Show / follow guest UART log")
    p_log.add_argument("--no-follow", action="store_false", dest="follow")
    p_bootlog = sub.add_parser("bootlog", help="Print complete UART/QEMU logs and run manifest")
    p_bootlog.add_argument("--history", action="store_true",
                           help="also list archived previous boot-log directories")

    p_screen = sub.add_parser("screenshot", help="Capture and verify the QEMU display via QMP")
    p_screen.add_argument("path", nargs="?", default=None,
                          help="optional PNG path under <build-root>/runs/<arch>/<mode>/")

    p_mouse = sub.add_parser("mouse", help="Move guest pointer; optionally click")
    p_mouse.add_argument("dx", type=int, help="relative horizontal movement")
    p_mouse.add_argument("dy", type=int, help="relative vertical movement")
    p_mouse.add_argument("--click", action="store_true", help="click after moving")
    p_mouse.add_argument("--button", choices=["left", "middle", "right"], default="left")
    p_mouse.add_argument("--delay", type=float, default=0.12,
                         help="press/release separation in seconds (0.01..1.0)")

    p_test = sub.add_parser("test", help="Full test gate")
    p_test.add_argument("-c", "--cmd", action="append", default=[], dest="cmds")
    p_test.add_argument("-e", "--expect", action="append", default=[])
    p_test.add_argument("-n", "--runs", type=int, default=1)
    p_test.add_argument("--soak-seconds", type=int, default=0,
                        help="supervise the guest for N seconds and capture screens")
    p_test.add_argument(
        "--timerdiag", action="store_true",
        help="compatibility option; timer diagnostics are now always in the full gate",
    )
    p_test.add_argument("--display", choices=["none", "default"], default="none",
                        help="QEMU display mode for screen/input verification")

    args = parser.parse_args()

    script = Path(__file__).resolve()
    root = script.parent.parent if script.parent.name == "tools" else script.parent
    os.chdir(root)
    session = Session.create(
        args.arch, root, boot_mode=args.boot_mode, build_root=args.build_root,
        firmware=args.firmware,
    )
    lock_file = None
    lock_acquired = False

    try:
        # All commands that can start/stop QEMU or send guest input share one
        # per-architecture lock. This prevents concurrent nxadb/nxrun callers
        # from racing over the same QMP socket, serial log and guest prompt.
        if args.cmd in MUTATING_COMMANDS:
            lock_path = (
                session.build_root / "runtime" / "nxadb" /
                session.arch / "session.lock"
            )
            lock_path.parent.mkdir(parents=True, exist_ok=True)
            lock_file = lock_path.open("a+")
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
            session.display = args.display
            start_qemu(session)
        elif args.cmd == "stop":
            stop_qemu(session)
        elif args.cmd == "restart":
            session.display = args.display
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
        elif args.cmd == "bootlog":
            cmd_bootlog(session, history=args.history)
        elif args.cmd == "screenshot":
            return cmd_screenshot(session, args.path)
        elif args.cmd == "mouse":
            return cmd_mouse(session, args.dx, args.dy, args.click, args.button, args.delay)
        elif args.cmd == "test":
            if args.soak_seconds < 0 or args.soak_seconds > 3600:
                raise ValueError("--soak-seconds must be between 0 and 3600")
            session.display = args.display
            return cmd_test(
                session, args.cmds, args.expect, args.runs,
                args.soak_seconds,
            )
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