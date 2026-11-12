"""Regression tests for nxadb BIOS/UEFI/direct lane separation."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import nxadb


class NxadbFirmwareTests(unittest.TestCase):
    def test_firmware_defaults_and_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bios = nxadb.Session.create("amd64", root, "iso", firmware="auto")
            uefi = nxadb.Session.create("amd64", root, "iso", firmware="uefi")
            arm = nxadb.Session.create("aarch64", root, "iso", firmware="auto")
            direct = nxadb.Session.create("amd64", root, "direct")

            self.assertEqual(bios.firmware, "bios")
            self.assertEqual(bios.lane_name(), "iso-bios")
            self.assertEqual(bios.boot_iso().name, "grub-bios.iso")
            self.assertEqual(uefi.firmware, "uefi")
            self.assertEqual(uefi.lane_name(), "iso-uefi")
            self.assertEqual(uefi.boot_iso().name, "nexsos-amd64-uefi.iso")
            self.assertEqual(arm.firmware, "uefi")
            self.assertEqual(arm.lane_name(), "iso-uefi")
            self.assertEqual(direct.lane_name(), "direct")

    def test_lane_namespaces_do_not_alias(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sessions = (
                nxadb.Session.create("amd64", root, "iso", firmware="bios"),
                nxadb.Session.create("amd64", root, "iso", firmware="uefi"),
                nxadb.Session.create("amd64", root, "direct"),
            )
            self.assertEqual(len({item.tmp for item in sessions}), 3)
            self.assertEqual(len({item.qmp_sock for item in sessions}), 3)
            self.assertEqual(len({item.pid_file for item in sessions}), 3)

    def test_invalid_firmware_combinations_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(ValueError):
                nxadb.Session.create("aarch64", root, "iso", firmware="bios")
            with self.assertRaises(ValueError):
                nxadb.Session.create("amd64", root, "direct", firmware="uefi")

    def test_build_target_follows_lane(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cases = (
                ("amd64", "iso", "bios", "boot-iso"),
                ("amd64", "iso", "uefi", "boot-uefi"),
                ("aarch64", "iso", "uefi", "boot-iso"),
                ("amd64", "direct", "auto", "direct-artifacts"),
            )
            for arch, mode, firmware, target in cases:
                session = nxadb.Session.create(
                    arch, root, mode, firmware=firmware
                )
                with (
                    patch.object(session, "required_artifacts", return_value=[]),
                    patch.object(
                        nxadb.subprocess, "run",
                        return_value=SimpleNamespace(returncode=0),
                    ) as run,
                ):
                    nxadb.ensure_artifacts(session)
                command = run.call_args.args[0]
                self.assertEqual(command[-1], target)
                self.assertIn(f"ARCH={arch}", command)

    def test_qemu_builder_receives_firmware(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            session = nxadb.Session.create(
                "amd64", root, "iso", firmware="uefi"
            )
            with patch.object(
                nxadb, "build_qemu_command", return_value=["qemu-system-x86_64"]
            ) as build:
                nxadb.build_qemu_cmdline(session)
            self.assertEqual(build.call_args.kwargs["firmware"], "uefi")

    def test_shell_navigation_probe_replays_history(self) -> None:
        session = SimpleNamespace(qmp_sock=Path("/tmp/qmp"), log_size=lambda: 17)
        marker = "NXADB_NAV_UP_HISTORY"
        with (
            patch.object(nxadb, "run_shell_c", return_value=(0, marker + "\n")),
            patch.object(nxadb, "qmp_special") as special,
            patch.object(nxadb, "wait_for_prompt", return_value=marker + "\n"),
        ):
            healthy, _ = nxadb._shell_navigation_probe(session)
        self.assertTrue(healthy)
        self.assertEqual(
            [call.args[1] for call in special.call_args_list], ["up", "ret"]
        )

    def test_shell_navigation_probe_rejects_missing_replay(self) -> None:
        session = SimpleNamespace(qmp_sock=Path("/tmp/qmp"), log_size=lambda: 23)
        marker = "NXADB_NAV_UP_HISTORY"
        with (
            patch.object(nxadb, "run_shell_c", return_value=(0, marker + "\n")),
            patch.object(nxadb, "qmp_special"),
            patch.object(nxadb, "wait_for_prompt", return_value="NXShell:/home> "),
        ):
            healthy, _ = nxadb._shell_navigation_probe(session)
        self.assertFalse(healthy)


if __name__ == "__main__":
    unittest.main(verbosity=2)
