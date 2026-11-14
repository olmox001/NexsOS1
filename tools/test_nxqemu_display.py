"""Explicit-result tests for QEMU virtual-monitor configuration."""
from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path
from unittest.mock import patch

import nxqemu


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def make_files(root: Path) -> tuple[Path, Path, Path]:
    disk = root / "disk.img"
    kernel = root / "kernel.bin"
    dtb = root / "virt.dtb"
    disk.write_bytes(b"disk")
    kernel.write_bytes(b"kernel")
    dtb.write_bytes(b"dtb")
    return disk, kernel, dtb


def test_provider_defaults() -> None:
    help_text = (
        "  xres=<uint32>          -  (default: 1280)\n"
        "  yres=<uint32>          -  (default: 800)\n"
    )
    completed = subprocess.CompletedProcess(
        ["qemu", "-device", "virtio-gpu-device,help"],
        0, stdout=help_text, stderr=""
    )
    with patch.object(nxqemu.subprocess, "run", return_value=completed):
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop("NEXS_QEMU_DISPLAY_WIDTH", None)
            os.environ.pop("NEXS_QEMU_DISPLAY_HEIGHT", None)
            require(
                nxqemu.qemu_native_display_size(
                    "/usr/bin/qemu", "virtio-gpu-device"
                ) == (1280, 800),
                "QEMU provider defaults were not preserved",
            )


def test_environment_override() -> None:
    env = {
        "NEXS_QEMU_DISPLAY_WIDTH": "1600",
        "NEXS_QEMU_DISPLAY_HEIGHT": "900",
    }
    with patch.object(
        nxqemu, "_qemu_gpu_property_defaults", return_value=(1280, 800)
    ):
        with patch.dict(os.environ, env, clear=False):
            require(
                nxqemu.qemu_native_display_size(
                    "/usr/bin/qemu", "virtio-gpu-device"
                ) == (1600, 900),
                "virtual-monitor override was not applied",
            )


def test_invalid_override() -> None:
    with patch.object(
        nxqemu, "_qemu_gpu_property_defaults", return_value=(1280, 800)
    ):
        with patch.dict(
            os.environ,
            {"NEXS_QEMU_DISPLAY_WIDTH": "0",
             "NEXS_QEMU_DISPLAY_HEIGHT": "800"},
            clear=False,
        ):
            try:
                nxqemu.qemu_native_display_size(
                    "/usr/bin/qemu", "virtio-gpu-device"
                )
            except ValueError as error:
                require("[1, 8192]" in str(error),
                        "invalid display size failed with wrong error")
                return
    raise RuntimeError("invalid virtual-monitor width was accepted")


def test_gpu_device_commands() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        disk, kernel, dtb = make_files(Path(temporary))
        with patch.object(
            nxqemu.shutil, "which",
            side_effect=lambda name: f"/usr/bin/{name}",
        ):
            with patch.object(
                nxqemu, "qemu_native_display_size", return_value=(1280, 800)
            ):
                amd = nxqemu.build_qemu_command(
                    "amd64", None, disk, serial="none", display="none",
                    boot_mode="direct", kernel=kernel,
                )
                arm = nxqemu.build_qemu_command(
                    "aarch64", None, disk, serial="none", display="none",
                    boot_mode="direct", kernel=kernel, dtb=dtb,
                )
    require(
        "virtio-gpu-pci,id=nxgpu0,xres=1280,yres=800" in amd,
        "AMD64 virtual monitor was not configured explicitly",
    )
    require(
        "virtio-gpu-device,id=nxgpu0,xres=1280,yres=800" in arm,
        "AArch64 virtual monitor was not configured explicitly",
    )


def main() -> int:
    test_provider_defaults()
    test_environment_override()
    test_invalid_override()
    test_gpu_device_commands()
    print("nxqemu display-profile tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
