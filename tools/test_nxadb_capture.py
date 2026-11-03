#!/usr/bin/env python3
"""Regression tests for nxadb's lossless boot-capture lifecycle."""
from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import nxadb


class NxadbCaptureTests(unittest.TestCase):
    def test_start_archives_previous_logs_and_persists_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            session = nxadb.Session.create("aarch64", root)
            session.ser_log.write_text("old firmware transcript\n")
            session.qemu_err.write_text("old qemu stderr\n")
            session.qemu_out.write_text("old qemu stdout\n")

            with (
                patch.object(session, "is_running", return_value=False),
                patch.object(nxadb, "ensure_artifacts"),
                patch.object(nxadb, "build_qemu_cmdline",
                             return_value=["qemu-system-aarch64", "-m", "5G"]),
                patch.object(nxadb.subprocess, "Popen",
                             return_value=SimpleNamespace(pid=4242)),
            ):
                nxadb.start_qemu(session, wait=False)

            archives = list((root / "build/runs/aarch64").glob("boot-*"))
            self.assertEqual(len(archives), 1)
            archive = archives[0]
            self.assertEqual((archive / "serial.log").read_text(),
                             "old firmware transcript\n")
            self.assertEqual((archive / "qemu.stderr.log").read_text(),
                             "old qemu stderr\n")
            self.assertEqual((archive / "qemu.stdout.log").read_text(),
                             "old qemu stdout\n")

            live = json.loads(session.pid_file.read_text())
            self.assertEqual(live["pid"], 4242)
            self.assertIn("qemu_stdout", live["logs"])
            self.assertIn("qemu_stderr", live["logs"])
            manifests = list((root / "build/runs/aarch64").glob("run-*.json"))
            self.assertEqual(len(manifests), 1)
            self.assertEqual(json.loads(manifests[0].read_text())["command"],
                             ["qemu-system-aarch64", "-m", "5G"])

    def test_bootlog_prints_full_serial_and_qemu_channels(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            session = nxadb.Session.create("aarch64", root)
            session.ser_log.write_text("UEFI full boot start\n" + ("K" * 9000) +
                                       "\nNEXSOS_EXIT_BOOT_SERVICES\n")
            session.qemu_out.write_text("QEMU stdout diagnostic\n")
            session.qemu_err.write_text("QEMU stderr diagnostic\n")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                nxadb.cmd_bootlog(session)
            captured = output.getvalue()
            self.assertIn("UEFI full boot start", captured)
            self.assertIn("NEXSOS_EXIT_BOOT_SERVICES", captured)
            self.assertIn("Q" + "EMU stdout diagnostic", captured)
            self.assertIn("QEMU stderr diagnostic", captured)
            self.assertGreater(len(captured), 9000)


if __name__ == "__main__":
    unittest.main(verbosity=2)
