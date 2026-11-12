"""Explicit-result tests for nxqemu CPU-model timer qualification."""
from __future__ import annotations

import os
import tempfile
from pathlib import Path
from unittest.mock import patch

import nxqemu


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def make_files(root: Path) -> tuple[Path, Path]:
    disk = root / "disk.img"
    iso = root / "boot.iso"
    disk.write_bytes(b"disk")
    iso.write_bytes(b"iso")
    return disk, iso


def build(disk: Path, iso: Path) -> list[str]:
    with patch.object(
        nxqemu.shutil, "which", return_value="/usr/bin/qemu-system-x86_64"
    ):
        return nxqemu.build_qemu_command(
            "amd64", iso, disk, serial="none", display="none",
            boot_mode="iso", firmware="bios"
        )


def cpu_model(command: list[str]) -> str:
    index = command.index("-cpu")
    return command[index + 1]


def test_default_cpu() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        disk, iso = make_files(Path(temporary))
        with patch.dict(os.environ, {"NEXS_QEMU_SMP": "4"}, clear=False):
            os.environ.pop("NEXS_QEMU_AMD64_CPU", None)
            require(cpu_model(build(disk, iso)) == "max",
                    "default amd64 CPU must remain max")


def test_override_cpu() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        disk, iso = make_files(Path(temporary))
        env = {"NEXS_QEMU_SMP": "4",
               "NEXS_QEMU_AMD64_CPU": "max,+tsc-deadline"}
        with patch.dict(os.environ, env, clear=False):
            require(cpu_model(build(disk, iso)) == "max,+tsc-deadline",
                    "amd64 CPU override was not forwarded")


def test_empty_override() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        disk, iso = make_files(Path(temporary))
        env = {"NEXS_QEMU_SMP": "4", "NEXS_QEMU_AMD64_CPU": "   "}
        with patch.dict(os.environ, env, clear=False):
            try:
                build(disk, iso)
            except ValueError as error:
                require("must not be empty" in str(error),
                        "empty override failed with the wrong error")
                return
        raise RuntimeError("empty amd64 CPU override was accepted")


def main() -> int:
    test_default_cpu()
    test_override_cpu()
    test_empty_override()
    print("nxqemu CPU-model tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
