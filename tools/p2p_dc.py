#!/usr/bin/env python3
"""Bounded local P2P console for Desktop Commander coordination.

Protocol v2 uses newline-delimited JSON frames over a local Unix-domain socket.
Messages are sequenced and persisted per named session so reconnecting clients
can replay history. The interactive client owns terminal line editing, allowing
incoming notifications to redraw without corrupting text being typed.

Runtime state is stored under .git/p2p-dc and is never part of the repository.
The service does not run an autonomous model and does not modify project files.
"""

from __future__ import annotations

import argparse
import codecs
from dataclasses import dataclass, field
from datetime import datetime
import fcntl
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import socket
import sys
import termios
import time
import tty
from typing import Optional

PROTOCOL_NAME = "nexs-p2p"
PROTOCOL_VERSION = 2

MAX_CLIENTS = 4
MAX_MESSAGE_BYTES = 4096
MAX_FRAME_BYTES = 8192
MAX_PEER_BUFFER_BYTES = 32768
MAX_FRAMES_PER_TICK = 64
MAX_HISTORY_RECORDS = 4096
MAX_HISTORY_SCAN_RECORDS = 8192
MAX_HISTORY_BYTES = 4 * 1024 * 1024
HISTORY_TRIM_RECORDS = 2048
MAX_REPLAY_RECORDS = 200
DEFAULT_REPLAY_RECORDS = 30
MAX_SERVER_CYCLES = 4_320_000
MAX_USER_CYCLES = 20_000_000
MAX_INPUT_CHARS_PER_TICK = 4096
MAX_SESSION_LEN = 64
MAX_NAME_LEN = 64
POLL_SECONDS = 0.10

SESSION_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")

ROOT = Path(__file__).resolve().parent.parent
STATE_DIR = ROOT / ".git" / "p2p-dc"
SOCKET_PATH = STATE_DIR / "chat.sock"
PID_PATH = STATE_DIR / "server.pid"
SEQ_PATH = STATE_DIR / "sequence"
COUNT_PATH = STATE_DIR / "history.count"
HISTORY_PATH = STATE_DIR / "history.jsonl"
LOCK_PATH = STATE_DIR / "history.lock"


@dataclass
class Peer:
    sock: socket.socket
    buffer: bytearray = field(default_factory=bytearray)
    session: str = ""
    name: str = ""
    ready: bool = False


@dataclass
class LineEditor:
    text: list[str] = field(default_factory=list)
    cursor: int = 0
    escape: str = ""
    command_history: list[str] = field(default_factory=list)
    history_index: int = 0

    def value(self) -> str:
        return "".join(self.text)

    def clear(self) -> None:
        self.text.clear()
        self.cursor = 0
        self.escape = ""
        self.history_index = len(self.command_history)

    def add_history(self, value: str) -> None:
        if value and (not self.command_history or self.command_history[-1] != value):
            self.command_history.append(value)
            if len(self.command_history) > 200:
                self.command_history = self.command_history[-200:]
        self.history_index = len(self.command_history)

    def select_history(self, direction: int) -> None:
        if not self.command_history:
            return
        target = self.history_index + direction
        if target < 0:
            target = 0
        if target > len(self.command_history):
            target = len(self.command_history)
        self.history_index = target
        if target == len(self.command_history):
            self.text = []
        else:
            self.text = list(self.command_history[target])
        self.cursor = len(self.text)

    def feed(self, ch: str) -> tuple[str, Optional[str]]:
        if self.escape:
            self.escape += ch
            action = self._finish_escape()
            if action != "pending":
                self.escape = ""
                return action, None
            if len(self.escape) >= 8:
                self.escape = ""
                return "redraw", None
            return "none", None

        if ch == "\x1b":
            self.escape = ch
            return "none", None
        if ch in ("\r", "\n"):
            value = self.value()
            self.add_history(value)
            self.clear()
            return "submit", value
        if ch in ("\x03", "\x04"):
            return "quit", None
        if ch in ("\x7f", "\b"):
            if self.cursor > 0:
                del self.text[self.cursor - 1]
                self.cursor -= 1
            return "redraw", None
        if ch == "\x01":
            self.cursor = 0
            return "redraw", None
        if ch == "\x05":
            self.cursor = len(self.text)
            return "redraw", None
        if ch == "\x15":
            self.text = []
            self.cursor = 0
            return "redraw", None
        if ch == "\x0b":
            del self.text[self.cursor:]
            return "redraw", None
        if ch.isprintable():
            self.text.insert(self.cursor, ch)
            self.cursor += 1
            return "redraw", None
        return "none", None

    def _finish_escape(self) -> str:
        mapping = {
            "\x1b[D": "left",
            "\x1b[C": "right",
            "\x1b[H": "home",
            "\x1b[F": "end",
            "\x1b[3~": "delete",
            "\x1b[A": "history_up",
            "\x1b[B": "history_down",
        }
        if self.escape in mapping:
            action = mapping[self.escape]
            if action == "left" and self.cursor > 0:
                self.cursor -= 1
            elif action == "right" and self.cursor < len(self.text):
                self.cursor += 1
            elif action == "home":
                self.cursor = 0
            elif action == "end":
                self.cursor = len(self.text)
            elif action == "delete" and self.cursor < len(self.text):
                del self.text[self.cursor]
            elif action == "history_up":
                self.select_history(-1)
            elif action == "history_down":
                self.select_history(1)
            return "redraw"

        prefixes = ("\x1b[", "\x1b[3")
        if any(prefix.startswith(self.escape) or self.escape.startswith(prefix)
               for prefix in prefixes):
            return "pending"
        return "redraw"


def ensure_state() -> None:
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    HISTORY_PATH.touch(exist_ok=True)
    LOCK_PATH.touch(exist_ok=True)


def timestamp() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def read_int(path: Path) -> int:
    try:
        value = int(path.read_text(encoding="utf-8").strip())
    except (FileNotFoundError, ValueError):
        return 0
    return value if value >= 0 else 0


def write_int(path: Path, value: int) -> None:
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(f"{value}\n", encoding="utf-8")
    os.replace(temp, path)


def validate_session(value: str) -> str:
    if not SESSION_RE.fullmatch(value):
        raise ValueError(
            f"session must match [A-Za-z0-9._-] and be 1..{MAX_SESSION_LEN} chars"
        )
    return value


def validate_name(value: str) -> str:
    name = value.strip()
    if not name:
        return "user"
    encoded = name.encode("utf-8")
    if len(encoded) > MAX_NAME_LEN:
        raise ValueError(f"name exceeds {MAX_NAME_LEN} UTF-8 bytes")
    return name


def validate_text(value: object) -> str:
    if not isinstance(value, str):
        raise ValueError("message text must be a string")
    text = value.rstrip("\r\n")
    encoded = text.encode("utf-8")
    if not text:
        raise ValueError("empty message")
    if len(encoded) > MAX_MESSAGE_BYTES:
        raise ValueError(f"message exceeds {MAX_MESSAGE_BYTES} UTF-8 bytes")
    return text


def pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def read_pid() -> int:
    return read_int(PID_PATH)


def clean_runtime() -> None:
    SOCKET_PATH.unlink(missing_ok=True)
    PID_PATH.unlink(missing_ok=True)


def encode_frame(frame: dict[str, object]) -> bytes:
    payload = json.dumps(
        frame, ensure_ascii=False, separators=(",", ":")
    ).encode("utf-8") + b"\n"
    if len(payload) > MAX_FRAME_BYTES:
        raise ValueError(f"protocol frame exceeds {MAX_FRAME_BYTES} bytes")
    return payload


def send_frame(sock: socket.socket, frame: dict[str, object]) -> None:
    sock.sendall(encode_frame(frame))


def read_history(
    session: Optional[str] = None,
    after_seq: int = 0,
    limit: int = DEFAULT_REPLAY_RECORDS,
) -> list[dict[str, object]]:
    ensure_state()
    bounded_limit = max(1, min(limit, MAX_REPLAY_RECORDS))
    selected: list[dict[str, object]] = []
    with HISTORY_PATH.open("r", encoding="utf-8") as stream:
        for index, line in enumerate(stream):
            if index >= MAX_HISTORY_SCAN_RECORDS:
                break
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            if not isinstance(record, dict):
                continue
            seq = record.get("seq")
            if not isinstance(seq, int) or seq <= after_seq:
                continue
            if session is not None and record.get("session") != session:
                continue
            selected.append(record)
            if len(selected) > bounded_limit:
                selected = selected[-bounded_limit:]
    return selected


def trim_history_locked() -> None:
    try:
        raw = HISTORY_PATH.read_bytes()
    except OSError:
        return
    if len(raw) > MAX_HISTORY_BYTES + MAX_FRAME_BYTES:
        raw = raw[-(MAX_HISTORY_BYTES + MAX_FRAME_BYTES):]
        first_newline = raw.find(b"\n")
        if first_newline >= 0:
            raw = raw[first_newline + 1:]

    lines = raw.splitlines()
    if len(lines) > HISTORY_TRIM_RECORDS:
        lines = lines[-HISTORY_TRIM_RECORDS:]

    temp = HISTORY_PATH.with_suffix(".jsonl.tmp")
    with temp.open("wb") as stream:
        for line in lines:
            stream.write(line + b"\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, HISTORY_PATH)
    write_int(COUNT_PATH, len(lines))


def append_history(role: str, session: str, text: str) -> dict[str, object]:
    ensure_state()
    session = validate_session(session)
    text = validate_text(text)
    if role not in ("user", "agent"):
        raise ValueError("history role must be user or agent")

    with LOCK_PATH.open("r+", encoding="utf-8") as lock_stream:
        fcntl.flock(lock_stream.fileno(), fcntl.LOCK_EX)
        seq = read_int(SEQ_PATH) + 1
        count = read_int(COUNT_PATH) + 1
        record: dict[str, object] = {
            "seq": seq,
            "ts": timestamp(),
            "session": session,
            "role": role,
            "text": text,
        }
        line = json.dumps(record, ensure_ascii=False, separators=(",", ":"))
        with HISTORY_PATH.open("a", encoding="utf-8") as stream:
            stream.write(line + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        write_int(SEQ_PATH, seq)
        write_int(COUNT_PATH, count)

        try:
            size = HISTORY_PATH.stat().st_size
        except OSError:
            size = 0
        if count > MAX_HISTORY_RECORDS or size > MAX_HISTORY_BYTES:
            trim_history_locked()
        fcntl.flock(lock_stream.fileno(), fcntl.LOCK_UN)
    return record


def safe_display(text: object) -> str:
    if not isinstance(text, str):
        return json.dumps(text, ensure_ascii=False)
    return json.dumps(text, ensure_ascii=False)


def print_record(prefix: str, record: dict[str, object]) -> None:
    session = record.get("session", "?")
    seq = record.get("seq", "?")
    role = str(record.get("role", "?")).upper()
    body = safe_display(record.get("text", ""))
    print(f"{prefix}[{session}][#{seq}][{role}] {body}", flush=True)


def peer_frames(peer: Peer, chunk: bytes) -> tuple[list[dict[str, object]], bool]:
    if len(peer.buffer) + len(chunk) > MAX_PEER_BUFFER_BYTES:
        return [], False
    peer.buffer.extend(chunk)
    frames: list[dict[str, object]] = []
    for _index in range(MAX_FRAMES_PER_TICK):
        newline = peer.buffer.find(b"\n")
        if newline < 0:
            break
        raw = bytes(peer.buffer[:newline])
        del peer.buffer[:newline + 1]
        if not raw:
            continue
        if len(raw) > MAX_FRAME_BYTES:
            return frames, False
        try:
            value = json.loads(raw.decode("utf-8", errors="strict"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return frames, False
        if not isinstance(value, dict):
            return frames, False
        frames.append(value)
    return frames, True


def broadcast_record(peers: list[Peer], record: dict[str, object]) -> list[Peer]:
    live: list[Peer] = []
    target_session = record.get("session")
    for peer in peers[:MAX_CLIENTS]:
        if not peer.ready or peer.session != target_session:
            live.append(peer)
            continue
        try:
            send_frame(peer.sock, {"type": "message", "record": record})
            live.append(peer)
        except OSError:
            try:
                peer.sock.close()
            except OSError:
                pass
    return live


def send_history(peer: Peer, limit: int) -> None:
    records = read_history(session=peer.session, limit=limit)
    send_frame(peer.sock, {
        "type": "history_begin",
        "session": peer.session,
        "count": len(records),
    })
    for record in records[:MAX_REPLAY_RECORDS]:
        send_frame(peer.sock, {"type": "history", "record": record})
    send_frame(peer.sock, {
        "type": "history_end",
        "session": peer.session,
        "count": len(records),
    })


def server_status(peers: list[Peer], active_session: str) -> dict[str, object]:
    sessions = sorted({
        peer.session for peer in peers[:MAX_CLIENTS] if peer.ready and peer.session
    })
    return {
        "type": "status",
        "protocol": PROTOCOL_VERSION,
        "pid": os.getpid(),
        "active_session": active_session,
        "clients": sum(1 for peer in peers[:MAX_CLIENTS] if peer.ready),
        "sessions": sessions,
        "last_seq": read_int(SEQ_PATH),
    }


def handle_peer_frame(
    peer: Peer,
    frame: dict[str, object],
    peers: list[Peer],
    active_session: str,
) -> tuple[str, bool]:
    frame_type = frame.get("type")
    if not peer.ready:
        if frame_type != "hello":
            send_frame(peer.sock, {"type": "error", "error": "hello required"})
            return active_session, False
        try:
            peer.session = validate_session(str(frame.get("session", "main")))
            peer.name = validate_name(str(frame.get("name", "user")))
        except ValueError as exc:
            send_frame(peer.sock, {"type": "error", "error": str(exc)})
            return active_session, False
        if frame.get("protocol") != PROTOCOL_VERSION:
            send_frame(peer.sock, {
                "type": "error",
                "error": f"protocol {PROTOCOL_VERSION} required",
            })
            return active_session, False
        peer.ready = True
        active_session = peer.session
        send_frame(peer.sock, {
            "type": "welcome",
            "protocol": PROTOCOL_VERSION,
            "session": peer.session,
            "server_pid": os.getpid(),
        })
        send_history(peer, DEFAULT_REPLAY_RECORDS)
        print(
            f"[P2P][{peer.session}] client connected name={safe_display(peer.name)} "
            f"fd={peer.sock.fileno()}",
            flush=True,
        )
        return active_session, True

    if frame_type == "message":
        try:
            text = validate_text(frame.get("text"))
            record = append_history("user", peer.session, text)
        except ValueError as exc:
            send_frame(peer.sock, {"type": "error", "error": str(exc)})
            return active_session, True
        active_session = peer.session
        print_record("[P2P]", record)
        send_frame(peer.sock, {"type": "ack", "seq": record["seq"]})
        return active_session, True

    if frame_type == "history":
        requested = frame.get("limit", DEFAULT_REPLAY_RECORDS)
        limit = requested if isinstance(requested, int) else DEFAULT_REPLAY_RECORDS
        send_history(peer, max(1, min(limit, MAX_REPLAY_RECORDS)))
        return active_session, True

    if frame_type == "status":
        send_frame(peer.sock, server_status(peers, active_session))
        return active_session, True

    if frame_type == "ping":
        send_frame(peer.sock, {"type": "pong", "ts": timestamp()})
        return active_session, True

    if frame_type == "bye":
        return active_session, False

    send_frame(peer.sock, {"type": "error", "error": "unknown frame type"})
    return active_session, True


def parse_agent_target(line: str, active_session: str) -> tuple[str, str]:
    if line.startswith("/to "):
        parts = line.split(" ", 2)
        if len(parts) != 3:
            raise ValueError("usage: /to SESSION MESSAGE")
        return validate_session(parts[1]), validate_text(parts[2])
    return active_session, validate_text(line)


def server_help() -> None:
    print(
        "[P2P] agent commands: /sessions | /status | /history [SESSION] [N] | "
        "/session SESSION | /to SESSION MESSAGE | /stop",
        flush=True,
    )


def handle_agent_line(
    line: str,
    peers: list[Peer],
    active_session: str,
) -> tuple[str, bool, list[Peer]]:
    text = line.rstrip("\r\n")
    if not text:
        return active_session, False, peers

    if text == "/stop":
        return active_session, True, peers
    if text == "/help":
        server_help()
        return active_session, False, peers
    if text == "/status":
        print(json.dumps(server_status(peers, active_session), ensure_ascii=False),
              flush=True)
        return active_session, False, peers
    if text == "/sessions":
        connected = sorted({
            peer.session for peer in peers[:MAX_CLIENTS] if peer.ready
        })
        print(
            f"[P2P] active={active_session} connected={connected}",
            flush=True,
        )
        return active_session, False, peers
    if text.startswith("/session "):
        try:
            active_session = validate_session(text.split(" ", 1)[1].strip())
            print(f"[P2P] active session={active_session}", flush=True)
        except ValueError as exc:
            print(f"[P2P ERROR] {exc}", flush=True)
        return active_session, False, peers
    if text.startswith("/history"):
        parts = text.split()
        session = active_session
        limit = DEFAULT_REPLAY_RECORDS
        if len(parts) >= 2:
            session = parts[1]
        if len(parts) >= 3:
            try:
                limit = int(parts[2])
            except ValueError:
                limit = DEFAULT_REPLAY_RECORDS
        try:
            session = validate_session(session)
        except ValueError as exc:
            print(f"[P2P ERROR] {exc}", flush=True)
            return active_session, False, peers
        records = read_history(session=session, limit=limit)
        print(f"[P2P] history session={session} count={len(records)}", flush=True)
        for record in records[:MAX_REPLAY_RECORDS]:
            print_record("[P2P HISTORY]", record)
        return active_session, False, peers

    try:
        session, body = parse_agent_target(text, active_session)
        record = append_history("agent", session, body)
    except ValueError as exc:
        print(f"[P2P ERROR] {exc}", flush=True)
        return active_session, False, peers

    print_record("[P2P]", record)
    peers = broadcast_record(peers, record)
    return session, False, peers


def serve() -> int:
    ensure_state()
    previous = read_pid()
    if pid_alive(previous):
        print(f"P2P server already running pid={previous}", flush=True)
        return 1

    clean_runtime()
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(SOCKET_PATH))
    os.chmod(SOCKET_PATH, 0o600)
    server.listen(MAX_CLIENTS)
    server.setblocking(False)
    write_int(PID_PATH, os.getpid())

    stopping = False

    def request_stop(_signum: int, _frame: object) -> None:
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)

    peers: list[Peer] = []
    active_session = "main"
    print(
        f"P2P READY protocol={PROTOCOL_VERSION} pid={os.getpid()} "
        f"socket={SOCKET_PATH}",
        flush=True,
    )
    server_help()

    try:
        for _cycle in range(MAX_SERVER_CYCLES):
            if stopping:
                break
            readers: list[object] = [server, sys.stdin]
            readers.extend(peer.sock for peer in peers[:MAX_CLIENTS])
            try:
                ready, _, _ = select.select(readers, [], [], POLL_SECONDS)
            except (OSError, ValueError):
                continue

            if server in ready:
                try:
                    client, _ = server.accept()
                    client.setblocking(False)
                    if len(peers) < MAX_CLIENTS:
                        peers.append(Peer(sock=client))
                    else:
                        send_frame(client, {"type": "error", "error": "server full"})
                        client.close()
                except OSError:
                    pass

            if sys.stdin in ready:
                line = sys.stdin.readline()
                if line == "":
                    stopping = True
                else:
                    active_session, requested_stop, peers = handle_agent_line(
                        line, peers, active_session
                    )
                    if requested_stop:
                        stopping = True

            next_peers: list[Peer] = []
            for peer in peers[:MAX_CLIENTS]:
                if peer.sock not in ready:
                    next_peers.append(peer)
                    continue
                try:
                    chunk = peer.sock.recv(MAX_FRAME_BYTES)
                except OSError:
                    chunk = b""
                if not chunk:
                    try:
                        peer.sock.close()
                    except OSError:
                        pass
                    if peer.ready:
                        print(
                            f"[P2P][{peer.session}] client disconnected "
                            f"name={safe_display(peer.name)}",
                            flush=True,
                        )
                    continue

                frames, valid = peer_frames(peer, chunk)
                keep = valid
                for frame in frames[:MAX_FRAMES_PER_TICK]:
                    if not keep:
                        break
                    try:
                        active_session, keep = handle_peer_frame(
                            peer, frame, peers, active_session
                        )
                    except (OSError, ValueError):
                        keep = False
                if keep:
                    next_peers.append(peer)
                else:
                    try:
                        peer.sock.close()
                    except OSError:
                        pass
            peers = next_peers
    finally:
        for peer in peers[:MAX_CLIENTS]:
            try:
                peer.sock.close()
            except OSError:
                pass
        try:
            server.close()
        except OSError:
            pass
        clean_runtime()
        print("[P2P] stopped", flush=True)
    return 0


def prompt_view(editor: LineEditor, columns: int) -> tuple[str, int]:
    """Render input inside one physical terminal row with horizontal scrolling."""
    prefix = "you> "
    width = max(20, columns)
    visible_chars = max(1, width - len(prefix) - 1)
    value = editor.value()

    start = max(0, editor.cursor - visible_chars + 1)
    if len(value) <= visible_chars:
        start = 0
    elif start > 0 and len(value) - start < visible_chars:
        start = max(0, len(value) - visible_chars)
    if editor.cursor < start:
        start = editor.cursor

    segment = value[start:start + visible_chars]
    local_cursor = max(0, min(editor.cursor - start, len(segment)))
    move_left = len(segment) - local_cursor
    return prefix + segment, move_left


def redraw_prompt(editor: LineEditor) -> None:
    columns = shutil.get_terminal_size((80, 24)).columns
    display, move_left = prompt_view(editor, columns)
    sys.stdout.write("\r\x1b[2K" + display)
    if move_left > 0:
        sys.stdout.write(f"\x1b[{move_left}D")
    sys.stdout.flush()


def client_notice(message: str, editor: LineEditor) -> None:
    sys.stdout.write("\r\x1b[2K" + message + "\n")
    redraw_prompt(editor)


def format_client_frame(frame: dict[str, object]) -> Optional[str]:
    frame_type = frame.get("type")
    if frame_type in ("history", "message"):
        record = frame.get("record")
        if not isinstance(record, dict):
            return "[protocol error: malformed record]"
        role = str(record.get("role", "?")).upper()
        seq = record.get("seq", "?")
        session = record.get("session", "?")
        tag = "history" if frame_type == "history" else "live"
        return (
            f"[{tag}][{session}][#{seq}][{role}] "
            f"{record.get('text', '')}"
        )
    if frame_type == "welcome":
        return (
            f"[connected] session={frame.get('session')} "
            f"protocol={frame.get('protocol')} pid={frame.get('server_pid')}"
        )
    if frame_type == "history_begin":
        return None
    if frame_type == "history_end":
        return f"[history replay] {frame.get('count', 0)} message(s)"
    if frame_type == "ack":
        return f"[sent #{frame.get('seq', '?')}]"
    if frame_type == "status":
        return (
            f"[status] active={frame.get('active_session')} "
            f"clients={frame.get('clients')} sessions={frame.get('sessions')} "
            f"last_seq={frame.get('last_seq')}"
        )
    if frame_type == "error":
        return f"[server error] {frame.get('error', 'unknown')}"
    if frame_type == "pong":
        return f"[pong] {frame.get('ts', '')}"
    return f"[protocol] {json.dumps(frame, ensure_ascii=False)}"


def handle_client_command(
    client: socket.socket,
    text: str,
    session: str,
) -> tuple[bool, bool]:
    if text == "/quit":
        try:
            send_frame(client, {"type": "bye"})
        except OSError:
            pass
        return True, False
    if text == "/help":
        print(
            "Commands: /quit /history [N] /status /ping. "
            "Editing: arrows, Home/End, Delete, Ctrl-A/E/U/K.",
            flush=True,
        )
        return False, True
    if text.startswith("/history"):
        parts = text.split()
        limit = DEFAULT_REPLAY_RECORDS
        if len(parts) >= 2:
            try:
                limit = int(parts[1])
            except ValueError:
                limit = DEFAULT_REPLAY_RECORDS
        send_frame(client, {
            "type": "history",
            "limit": max(1, min(limit, MAX_REPLAY_RECORDS)),
        })
        return False, False
    if text == "/status":
        send_frame(client, {"type": "status"})
        return False, False
    if text == "/ping":
        send_frame(client, {"type": "ping"})
        return False, False
    if not text:
        return False, False
    try:
        body = validate_text(text)
    except ValueError as exc:
        print(f"[input error] {exc}", flush=True)
        return False, True
    send_frame(client, {
        "type": "message",
        "session": session,
        "text": body,
    })
    return False, False


def user_console(session: str, name: str) -> int:
    ensure_state()
    session = validate_session(session)
    name = validate_name(name)
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        print("interactive user mode requires a TTY", file=sys.stderr)
        return 2

    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        client.connect(str(SOCKET_PATH))
    except OSError as exc:
        print(f"cannot connect: {exc}", file=sys.stderr)
        return 1
    client.setblocking(False)
    send_frame(client, {
        "type": "hello",
        "protocol": PROTOCOL_VERSION,
        "session": session,
        "name": name,
    })

    fd = sys.stdin.fileno()
    original = termios.tcgetattr(fd)
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    editor = LineEditor()
    socket_buffer = Peer(sock=client)
    should_quit = False

    print(
        f"NexsOS1 P2P session={session}. /help for commands; /quit exits.",
        flush=True,
    )
    try:
        tty.setcbreak(fd)
        redraw_prompt(editor)
        for _cycle in range(MAX_USER_CYCLES):
            if should_quit:
                break
            try:
                ready, _, _ = select.select([fd, client], [], [], POLL_SECONDS)
            except (OSError, ValueError):
                continue

            if client in ready:
                try:
                    chunk = client.recv(MAX_FRAME_BYTES)
                except OSError:
                    chunk = b""
                if not chunk:
                    client_notice("[server disconnected]", editor)
                    break
                frames, valid = peer_frames(socket_buffer, chunk)
                if not valid:
                    client_notice("[protocol error: invalid frame]", editor)
                    break
                for frame in frames[:MAX_FRAMES_PER_TICK]:
                    message = format_client_frame(frame)
                    if message is not None:
                        client_notice(message, editor)

            if fd in ready:
                try:
                    raw = os.read(fd, MAX_INPUT_CHARS_PER_TICK)
                except OSError:
                    raw = b""
                if not raw:
                    break
                chars = decoder.decode(raw)
                for ch in chars[:MAX_INPUT_CHARS_PER_TICK]:
                    action, value = editor.feed(ch)
                    if action == "quit":
                        should_quit = True
                        break
                    if action == "submit" and value is not None:
                        sys.stdout.write("\r\x1b[2K")
                        sys.stdout.flush()
                        if value and not value.startswith("/"):
                            print(
                                f"[you][{session}] {safe_display(value)}",
                                flush=True,
                            )
                        try:
                            should_quit, _ = handle_client_command(
                                client, value, session
                            )
                        except (OSError, ValueError) as exc:
                            print(f"[send error] {exc}", flush=True)
                            should_quit = True
                        if not should_quit:
                            redraw_prompt(editor)
                        continue
                    if action == "redraw":
                        redraw_prompt(editor)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, original)
        sys.stdout.write("\r\x1b[2K")
        sys.stdout.flush()
        try:
            client.close()
        except OSError:
            pass
    return 0


def recv_until_ack(client: socket.socket, timeout_cycles: int = 80) -> int:
    peer = Peer(sock=client)
    for _cycle in range(timeout_cycles):
        ready, _, _ = select.select([client], [], [], 0.05)
        if client not in ready:
            continue
        chunk = client.recv(MAX_FRAME_BYTES)
        if not chunk:
            return 1
        frames, valid = peer_frames(peer, chunk)
        if not valid:
            return 1
        for frame in frames[:MAX_FRAMES_PER_TICK]:
            if frame.get("type") == "ack":
                print(f"queued #{frame.get('seq')}")
                return 0
            if frame.get("type") == "error":
                print(f"server error: {frame.get('error')}", file=sys.stderr)
                return 1
    print("ack timeout", file=sys.stderr)
    return 1


def send_once(session: str, name: str, message: str) -> int:
    ensure_state()
    session = validate_session(session)
    name = validate_name(name)
    body = validate_text(message)
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(2.0)
    try:
        client.connect(str(SOCKET_PATH))
        send_frame(client, {
            "type": "hello",
            "protocol": PROTOCOL_VERSION,
            "session": session,
            "name": name,
        })
        send_frame(client, {
            "type": "message",
            "session": session,
            "text": body,
        })
        client.setblocking(False)
        return recv_until_ack(client)
    except OSError as exc:
        print(f"send failed: {exc}", file=sys.stderr)
        return 1
    finally:
        client.close()


def show_history(session: str, limit: int) -> int:
    session = validate_session(session)
    records = read_history(
        session=session,
        limit=max(1, min(limit, MAX_REPLAY_RECORDS)),
    )
    print(f"session={session} count={len(records)}")
    for record in records[:MAX_REPLAY_RECORDS]:
        print_record("", record)
    return 0


def show_status() -> int:
    pid = read_pid()
    alive = pid_alive(pid)
    print(f"server={'running' if alive else 'stopped'} pid={pid if alive else '-'}")
    print(f"protocol={PROTOCOL_VERSION}")
    print(f"socket={SOCKET_PATH} exists={SOCKET_PATH.exists()}")
    print(f"history={HISTORY_PATH}")
    print(f"last_seq={read_int(SEQ_PATH)}")
    print(f"history_records={read_int(COUNT_PATH)}")
    return 0 if alive else 1


def stop_server() -> int:
    pid = read_pid()
    if not pid_alive(pid):
        clean_runtime()
        print("server already stopped")
        return 0
    os.kill(pid, signal.SIGTERM)
    for _cycle in range(60):
        time.sleep(0.05)
        if not pid_alive(pid):
            clean_runtime()
            print(f"server stopped pid={pid}")
            return 0
    print(f"server did not stop pid={pid}", file=sys.stderr)
    return 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Desktop Commander local P2P console")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("server")

    user_parser = sub.add_parser("user")
    user_parser.add_argument("--session", default="main")
    user_parser.add_argument("--name", default="owner")

    send_parser = sub.add_parser("send")
    send_parser.add_argument("--session", default="main")
    send_parser.add_argument("--name", default="owner")
    send_parser.add_argument("message", nargs="+")

    history_parser = sub.add_parser("history")
    history_parser.add_argument("--session", default="main")
    history_parser.add_argument("-n", "--limit", type=int, default=30)

    sub.add_parser("status")
    sub.add_parser("stop")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.command == "server":
            return serve()
        if args.command == "user":
            return user_console(args.session, args.name)
        if args.command == "send":
            return send_once(args.session, args.name, " ".join(args.message))
        if args.command == "history":
            return show_history(args.session, args.limit)
        if args.command == "status":
            return show_status()
        if args.command == "stop":
            return stop_server()
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
