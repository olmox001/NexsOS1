#!/usr/bin/env python3
"""Regression tests for the bounded Desktop Commander P2P protocol."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import socket
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "p2p_dc.py"


def load_module():
    spec = importlib.util.spec_from_file_location("p2p_dc_test_target", MODULE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load p2p_dc.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class P2PProtocolTests(unittest.TestCase):
    def setUp(self):
        self.module = load_module()
        self.temp = tempfile.TemporaryDirectory(prefix="p2p-dc-test-")
        state = Path(self.temp.name)
        self.module.STATE_DIR = state
        self.module.SOCKET_PATH = state / "chat.sock"
        self.module.PID_PATH = state / "server.pid"
        self.module.SEQ_PATH = state / "sequence"
        self.module.COUNT_PATH = state / "history.count"
        self.module.HISTORY_PATH = state / "history.jsonl"
        self.module.LOCK_PATH = state / "history.lock"

    def tearDown(self):
        self.temp.cleanup()

    def test_history_isolated_by_session_and_ordered(self):
        first = self.module.append_history("user", "main", "one")
        self.module.append_history("user", "other", "hidden")
        third = self.module.append_history("agent", "main", "two")

        records = self.module.read_history(session="main", limit=10)

        self.assertEqual([item["text"] for item in records], ["one", "two"])
        self.assertEqual([item["seq"] for item in records],
                         [first["seq"], third["seq"]])
        self.assertLess(first["seq"], third["seq"])

    def test_history_after_sequence_filters_old_records(self):
        first = self.module.append_history("user", "main", "one")
        second = self.module.append_history("agent", "main", "two")
        third = self.module.append_history("user", "main", "three")

        records = self.module.read_history(
            session="main", after_seq=first["seq"], limit=10
        )

        self.assertEqual([item["seq"] for item in records],
                         [second["seq"], third["seq"]])

    def test_fragmented_frame_waits_for_newline(self):
        left, right = socket.socketpair()
        try:
            peer = self.module.Peer(sock=left)
            frame = self.module.encode_frame({
                "type": "message",
                "session": "main",
                "text": "hello",
            })
            split = len(frame) // 2
            first, valid_first = self.module.peer_frames(peer, frame[:split])
            second, valid_second = self.module.peer_frames(peer, frame[split:])

            self.assertTrue(valid_first)
            self.assertEqual(first, [])
            self.assertTrue(valid_second)
            self.assertEqual(len(second), 1)
            self.assertEqual(second[0]["text"], "hello")
        finally:
            left.close()
            right.close()

    def test_oversized_peer_buffer_fails_closed(self):
        left, right = socket.socketpair()
        try:
            peer = self.module.Peer(sock=left)
            payload = b"x" * (self.module.MAX_PEER_BUFFER_BYTES + 1)
            frames, valid = self.module.peer_frames(peer, payload)
            self.assertEqual(frames, [])
            self.assertFalse(valid)
        finally:
            left.close()
            right.close()

    def test_line_editor_preserves_insert_delete_and_history(self):
        editor = self.module.LineEditor()
        for char in "abc":
            editor.feed(char)
        for char in "\x1b[D":
            editor.feed(char)
        editor.feed("X")
        self.assertEqual(editor.value(), "abXc")
        self.assertEqual(editor.cursor, 3)

        editor.feed("\x7f")
        self.assertEqual(editor.value(), "abc")
        self.assertEqual(editor.cursor, 2)

        action, value = editor.feed("\n")
        self.assertEqual((action, value), ("submit", "abc"))
        for char in "\x1b[A":
            editor.feed(char)
        self.assertEqual(editor.value(), "abc")

    def test_prompt_view_stays_on_one_physical_row(self):
        editor = self.module.LineEditor()
        for char in "abcdefghijklmnopqrstuvwxyz":
            editor.feed(char)

        display, move_left = self.module.prompt_view(editor, 20)

        self.assertLessEqual(len(display), 19)
        self.assertEqual(move_left, 0)
        for char in "\x1b[D" * 8:
            editor.feed(char)
        display, move_left = self.module.prompt_view(editor, 20)
        self.assertLessEqual(len(display), 19)
        self.assertGreaterEqual(move_left, 0)

    def test_role_colors_are_stable(self):
        agent = self.module.colorize_role("agent", "message")
        user = self.module.colorize_role("user", "message")
        other = self.module.colorize_role("system", "message")

        self.assertEqual(agent, "\x1b[31mmessage\x1b[0m")
        self.assertEqual(user, "\x1b[32mmessage\x1b[0m")
        self.assertEqual(other, "message")

    def test_text_and_session_bounds_reject_invalid_input(self):
        with self.assertRaises(ValueError):
            self.module.validate_session("../escape")
        with self.assertRaises(ValueError):
            self.module.validate_text(
                "x" * (self.module.MAX_MESSAGE_BYTES + 1)
            )


if __name__ == "__main__":
    unittest.main()
