#!/usr/bin/env python3
"""Shared, GRUB-first QEMU command construction for NexsOS1.

This module is imported by nxadb.py and is also the Makefile's run backend.
It deliberately never kills unrelated QEMU processes.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parents[1]


def clean_qemu_environment() -> dict[str, str]:
    env = os.environ.copy()
    for key in (
        "GTK_PATH", "GTK_EXE_PREFIX", "GTK_MODULES", "GTK_IM_MODULE_FILE",
        "GIO_MODULE_DIR", "GSETTINGS_SCHEMA_DIR", "XDG_DATA_DIRS",
        "XDG_DATA_HOME", "LOCPATH",
    ):
        env.pop(key, None)
    return env


def _first_file(paths: list[Path]) -> Optional[Path]:
    for path in paths:
        if path.is_file():
            return path
    return None


def find_qemu_efi_firmware() -> dict[str, Path | str]:
    """Find split AAVMF/EDK2 pflash images or a combined AArch64 EFI ROM."""
    env_code = os.environ.get("NEXS_QEMU_EFI_CODE") or os.environ.get("QEMU_EFI_CODE")
    env_vars = os.environ.get("NEXS_QEMU_EFI_VARS") or os.environ.get("QEMU_EFI_VARS")
    env_combined = os.environ.get("NEXS_QEMU_EFI_FIRMWARE") or os.environ.get("QEMU_EFI_FIRMWARE")

    code_candidates: list[Path] = []
    vars_candidates: list[Path] = []
    combined_candidates: list[Path] = []
    if env_code:
        code_candidates.append(Path(env_code).expanduser())
    if env_vars:
        vars_candidates.append(Path(env_vars).expanduser())
    if env_combined:
        combined_candidates.append(Path(env_combined).expanduser())

    for prefix in (Path("/usr/local/share/qemu"), Path("/opt/homebrew/share/qemu"),
                   Path("/usr/share/qemu")):
        code_candidates.append(prefix / "edk2-aarch64-code.fd")
        vars_candidates.append(prefix / "edk2-arm-vars.fd")

    for prefix in (Path("/usr/share/AAVMF"), Path("/usr/share/edk2/aarch64"),
                   Path("/usr/share/qemu-efi-aarch64"),
                   Path("/usr/lib/qemu"), Path("/usr/lib/edk2/aarch64")):
        code_candidates.extend((prefix / "AAVMF_CODE.fd", prefix / "QEMU_EFI_CODE.fd",
                                prefix / "QEMU_EFI.fd"))
        vars_candidates.extend((prefix / "AAVMF_VARS.fd", prefix / "QEMU_VARS.fd",
                                prefix / "vars-template-pflash.raw"))
        combined_candidates.extend((prefix / "QEMU_EFI.fd", prefix / "QEMU_EFI-pflash.raw"))

    # Homebrew's QEMU prefix is not necessarily /usr/local or /opt/homebrew.
    brew = shutil.which("brew")
    if brew:
        try:
            prefix = Path(subprocess.check_output([brew, "--prefix", "qemu"], text=True).strip())
            code_candidates.append(prefix / "share/qemu/edk2-aarch64-code.fd")
            vars_candidates.append(prefix / "share/qemu/edk2-arm-vars.fd")
        except (subprocess.CalledProcessError, OSError):
            pass

    code = _first_file(code_candidates)
    var_template = _first_file(vars_candidates)
    if code and var_template:
        return {"mode": "split", "code": code, "vars": var_template}

    combined = _first_file(combined_candidates)
    if combined:
        return {"mode": "combined", "firmware": combined}

    raise RuntimeError(
        "AArch64 UEFI firmware not found. Install QEMU EDK2/AAVMF firmware or set "
        "NEXS_QEMU_EFI_CODE and NEXS_QEMU_EFI_VARS (or NEXS_QEMU_EFI_FIRMWARE)."
    )


def prepare_uefi_vars(firmware: dict[str, Path | str], destination: Path,
                      fresh: bool = True) -> Optional[Path]:
    if firmware.get("mode") != "split":
        return None
    template = Path(str(firmware["vars"]))
    destination.parent.mkdir(parents=True, exist_ok=True)
    if fresh or not destination.exists():
        shutil.copy2(template, destination)
    return destination


def build_qemu_command(
    arch: str,
    iso: Optional[Path],
    disk: Path,
    serial: str = "stdio",
    display: str = "default",
    qmp_socket: Optional[Path] = None,
    debug: bool = False,
    uefi_vars_file: Optional[Path] = None,
    boot_mode: str = "iso",
    kernel: Optional[Path] = None,
    dtb: Optional[Path] = None,
) -> list[str]:
    arch = arch.lower()
    boot_mode = boot_mode.lower()
    if arch not in ("amd64", "aarch64"):
        raise ValueError(f"unsupported architecture: {arch}")
    if boot_mode not in ("iso", "direct"):
        raise ValueError(f"unsupported boot mode: {boot_mode}")

    disk = disk.resolve()
    if not disk.is_file():
        raise FileNotFoundError(
            f"root filesystem image not found: {disk}; run make ARCH={arch} disk"
        )
    if boot_mode == "iso":
        if iso is None:
            raise ValueError("GRUB ISO is required in iso mode")
        iso = iso.resolve()
        if not iso.is_file():
            raise FileNotFoundError(
                f"GRUB boot ISO not found: {iso}; run make ARCH={arch} boot-iso"
            )
    else:
        if kernel is None:
            raise ValueError("kernel image is required in direct mode")
        kernel = kernel.resolve()
        if not kernel.is_file():
            raise FileNotFoundError(f"kernel image not found: {kernel}")
        if arch == "aarch64":
            if dtb is None:
                raise ValueError("AArch64 direct mode requires a DTB")
            dtb = dtb.resolve()
            if not dtb.is_file():
                raise FileNotFoundError(f"device tree not found: {dtb}")

    qemu_name = "qemu-system-x86_64" if arch == "amd64" else "qemu-system-aarch64"
    qemu = shutil.which(qemu_name)
    if not qemu:
        raise RuntimeError(f"missing emulator: {qemu_name}")

    smp_count = int(os.environ.get("NEXS_QEMU_SMP", "4"))
    if not 1 <= smp_count <= 64:
        raise ValueError("NEXS_QEMU_SMP must be in [1, 64]")
    cmd = [qemu, "-m", "5G", "-smp", str(smp_count),
           "-no-reboot", "-no-shutdown"]
    if boot_mode == "iso":
        cmd += ["-boot", "order=d"]

    if arch == "amd64":
        cmd += ["-machine", "q35", "-cpu", "max"]
        if boot_mode == "iso":
            cmd += ["-cdrom", str(iso)]
        else:
            cmd += ["-kernel", str(kernel)]
        cmd += ["-device", "virtio-gpu-pci,id=nxgpu0",
                "-device", "virtio-keyboard-pci,id=nxkbd0,display=nxgpu0",
                "-device", "virtio-mouse-pci,id=nxmouse0,display=nxgpu0",
                "-drive", f"if=none,file={disk},id=rootfs,format=raw",
                "-device", "virtio-blk-pci,drive=rootfs"]
    else:
        cmd += ["-machine", "virt", "-cpu", "cortex-a57"]
        if boot_mode == "iso":
            firmware = find_qemu_efi_firmware()
            if firmware["mode"] == "split":
                if uefi_vars_file is None:
                    uefi_vars_file = ROOT / "build/aarch64/grub-efi/vars.fd"
                vars_path = prepare_uefi_vars(
                    firmware, uefi_vars_file, fresh=True
                )
                cmd += [
                    "-drive",
                    f"if=pflash,format=raw,unit=0,readonly=on,file={firmware['code']}",
                    "-drive",
                    f"if=pflash,format=raw,unit=1,file={vars_path}",
                ]
            else:
                cmd += ["-bios", str(firmware["firmware"])]
        else:
            cmd += ["-kernel", str(kernel), "-dtb", str(dtb)]
        cmd += ["-device", "virtio-gpu-device,id=nxgpu0",
                "-device", "virtio-keyboard-device,id=nxkbd0,display=nxgpu0",
                "-device", "virtio-mouse-device,id=nxmouse0,display=nxgpu0",
                "-drive", f"if=none,file={disk},id=rootfs,format=raw",
                "-device", "virtio-blk-device,drive=rootfs"]
        if boot_mode == "iso":
            cmd += ["-device", "virtio-scsi-device,id=scsi0",
                    "-drive",
                    f"if=none,file={iso},id=cdrom,media=cdrom,format=raw",
                    "-device", "scsi-cd,drive=cdrom"]

    if serial == "none":
        cmd += ["-serial", "none"]
    elif serial == "stdio":
        cmd += ["-serial", "stdio"]
    elif serial.startswith("file:"):
        serial_path = Path(serial[5:]).resolve()
        serial_path.parent.mkdir(parents=True, exist_ok=True)
        cmd += ["-serial", f"file:{serial_path}"]
    else:
        raise ValueError("serial must be 'stdio', 'none', or 'file:<path>'")

    if display != "default":
        cmd += ["-display", display]
    if qmp_socket is not None:
        qmp_socket = qmp_socket.resolve()
        qmp_socket.parent.mkdir(parents=True, exist_ok=True)
        cmd += ["-qmp", f"unix:{qmp_socket},server=on,wait=off"]
    if debug:
        cmd += ["-s", "-S"]
    return cmd


def main() -> int:
    parser = argparse.ArgumentParser(description="Run NexsOS1 under QEMU")
    parser.add_argument("--arch", choices=("amd64", "aarch64"), required=True)
    parser.add_argument("--boot-mode", choices=("iso", "direct"), default="iso")
    parser.add_argument("--iso", type=Path)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--dtb", type=Path)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--serial", default="stdio")
    parser.add_argument("--display", default="default")
    parser.add_argument("--qmp", type=Path)
    parser.add_argument("--uefi-vars", type=Path)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--print-command", action="store_true")
    args = parser.parse_args()
    try:
        command = build_qemu_command(
            args.arch, args.iso, args.disk, args.serial, args.display,
            args.qmp, args.debug, args.uefi_vars, args.boot_mode,
            args.kernel, args.dtb,
        )
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"nxqemu: {exc}", file=sys.stderr)
        return 2
    if args.print_command:
        import shlex
        print(shlex.join(command))
        return 0
    return subprocess.call(command, cwd=ROOT, env=clean_qemu_environment())


if __name__ == "__main__":
    raise SystemExit(main())
