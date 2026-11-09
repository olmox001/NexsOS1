#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_ROOT="$(cd "$ROOT/.." && pwd)"
GRUB_MANIFEST="$PROJECT_ROOT/boot/grub/manifest.env"
# shellcheck disable=SC1091
set -a
source "$ROOT/config/toolchain-versions.env"
source "$GRUB_MANIFEST"
set +a

BUILD_ROOT="${NEXS_BUILD_ROOT:-$PROJECT_ROOT/build}"
case "$BUILD_ROOT" in
    /*) ;;
    *) echo "[aarch64-uefi] NEXS_BUILD_ROOT must be absolute: $BUILD_ROOT" >&2; exit 1 ;;
esac
BUILD="$BUILD_ROOT/aarch64"
GRUB_STAGE="$BUILD_ROOT/cache/grub/prebuilt/arm64-efi"
GRUB_MKIMAGE="$GRUB_STAGE/bin/grub-mkimage"
GRUB_MODULE_DIR="$GRUB_STAGE/lib/grub/arm64-efi"
LOG_DIR="$BUILD_ROOT/logs"
EFI_ROOT="$BUILD/grub-efi"
ISO_STAGE="$EFI_ROOT/iso"
EFI_IMAGE="$EFI_ROOT/efi.img"
EFI_STUB="$BUILD/nexsos-efi-loader.efi"
ISO="$EFI_ROOT/nexsos-aarch64-uefi.iso"
KERNEL="${NEXS_KERNEL_IMAGE:-$BUILD/kernel.bin}"
DTB="${NEXS_DTB_PATH:-$BUILD/virt.dtb}"
HOST_CLANG="${NEXS_HOST_CLANG:-$(command -v clang || true)}"
LLD_LINK="${NEXS_LLD_LINK:-$(command -v lld-link || true)}"
if [[ -z "$LLD_LINK" ]]; then
    for candidate in /usr/local/opt/llvm/bin/lld-link /opt/homebrew/opt/llvm/bin/lld-link; do
        if [[ -x "$candidate" ]]; then LLD_LINK="$candidate"; break; fi
    done
fi
if [[ -z "$LLD_LINK" ]] && command -v brew >/dev/null 2>&1; then
    LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || true)"
    if [[ -n "$LLVM_PREFIX" && -x "$LLVM_PREFIX/bin/lld-link" ]]; then
        LLD_LINK="$LLVM_PREFIX/bin/lld-link"
    fi
fi
JOBS="${NEXS_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

for tool in xorriso mformat mmd mcopy python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "[aarch64-uefi] missing required tool: $tool" >&2
        exit 1
    }
done
[[ -n "$HOST_CLANG" && -x "$HOST_CLANG" ]] || { echo "[aarch64-uefi] clang is required" >&2; exit 1; }
[[ -n "$LLD_LINK" && -x "$LLD_LINK" ]] || { echo "[aarch64-uefi] lld-link is required" >&2; exit 1; }
[[ -s "$KERNEL" ]] || { echo "[aarch64-uefi] missing kernel image: $KERNEL (build kernel first)" >&2; exit 1; }
[[ -s "$DTB" ]] || { echo "[aarch64-uefi] missing Device Tree: $DTB (generate DTB first)" >&2; exit 1; }

"$ROOT/tools/build-grub-arm64.sh"
[[ -x "$GRUB_MKIMAGE" && -f "$GRUB_MODULE_DIR/chain.mod" ]] || {
    echo "[aarch64-uefi] reusable GRUB arm64-efi stage is incomplete" >&2
    exit 1
}

mkdir -p "$BUILD" "$EFI_ROOT" "$LOG_DIR"

EFI_CFLAGS=(
    --target=aarch64-pc-windows-msvc
    -ffreestanding -fno-stack-protector -fno-builtin -fno-exceptions
    -fno-unwind-tables -fno-asynchronous-unwind-tables -mgeneral-regs-only
    -O2 -Wall -Wextra -Werror
    -I"$ROOT/HAL" -I"$ROOT/HAL/arch/aarch64/include"
    -I"$ROOT/HAL/include" -I"$ROOT/include"
)

"$HOST_CLANG" "${EFI_CFLAGS[@]}" -std=c11 \
    -c "$ROOT/HAL/arch/aarch64/boot/uefi/efi_stub.c" \
    -o "$BUILD/efi_stub.obj"
"$HOST_CLANG" "${EFI_CFLAGS[@]}" -std=c11 \
    -c "$ROOT/HAL/arch/aarch64/boot/uefi/efi_exit_adapter.c" \
    -o "$BUILD/efi_exit_adapter.obj"
"$HOST_CLANG" "${EFI_CFLAGS[@]}" -std=c11 \
    -c "$ROOT/HAL/capability/uefi_exit.c" \
    -o "$BUILD/uefi_exit.obj"
"$HOST_CLANG" "${EFI_CFLAGS[@]}" -std=c11 \
    -c "$ROOT/HAL/capability/uefi_memory_map.c" \
    -o "$BUILD/uefi_memory_map.obj"
"$HOST_CLANG" --target=aarch64-pc-windows-msvc -ffreestanding \
    -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -c "$ROOT/HAL/arch/aarch64/boot/uefi/efi_handoff.S" \
    -o "$BUILD/efi_handoff.obj"
KERNEL_ABS="$(cd "$(dirname "$KERNEL")" && pwd)/$(basename "$KERNEL")"
DTB_ABS="$(cd "$(dirname "$DTB")" && pwd)/$(basename "$DTB")"
"$HOST_CLANG" --target=aarch64-pc-windows-msvc -ffreestanding \
    -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -DNEXS_EFI_KERNEL_IMAGE="\"$KERNEL_ABS\"" \
    -DNEXS_EFI_DTB_IMAGE="\"$DTB_ABS\"" \
    -c "$ROOT/HAL/arch/aarch64/boot/uefi/efi_payload.S" \
    -o "$BUILD/efi_payload.obj"
"$LLD_LINK" /subsystem:efi_application /entry:efi_main /nodefaultlib \
    /machine:arm64 /out:"$EFI_STUB" \
    "$BUILD/efi_stub.obj" "$BUILD/efi_exit_adapter.obj" \
    "$BUILD/uefi_exit.obj" "$BUILD/uefi_memory_map.obj" \
    "$BUILD/efi_handoff.obj" "$BUILD/efi_payload.obj"

# Recreate only generated EFI/ISO staging directories, never source directories.
case "$ISO_STAGE" in "$BUILD_ROOT"/aarch64/grub-efi/iso) ;; *) echo "unsafe ISO staging path: $ISO_STAGE" >&2; exit 1 ;; esac
rm -rf "$ISO_STAGE"
mkdir -p "$ISO_STAGE/boot/grub" "$EFI_ROOT/EFI/BOOT"

cat > "$EFI_ROOT/embedded.cfg" <<'GRUB_EMBEDDED'
search --no-floppy --file --set=root /boot/grub/grub.cfg
configfile /boot/grub/grub.cfg
GRUB_EMBEDDED

"$GRUB_MKIMAGE" -O arm64-efi \
    -d "$GRUB_MODULE_DIR" \
    -o "$EFI_ROOT/EFI/BOOT/BOOTAA64.EFI" \
    -p /boot/grub -c "$EFI_ROOT/embedded.cfg" \
    normal chain iso9660 search search_fs_file configfile echo \
    fat part_gpt help reboot halt

# UEFI El Torito boot image: FAT filesystem containing the GRUB AA64 loader.
python3 - "$EFI_IMAGE" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
p.parent.mkdir(parents=True, exist_ok=True)
with p.open("wb") as f:
    f.truncate(8 * 1024 * 1024)
PY
mformat -i "$EFI_IMAGE" -v NEXSAA64 ::
mmd -i "$EFI_IMAGE" ::/EFI
mmd -i "$EFI_IMAGE" ::/EFI/BOOT
mcopy -i "$EFI_IMAGE" "$EFI_ROOT/EFI/BOOT/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI

cp "$EFI_IMAGE" "$ISO_STAGE/boot/grub/efi.img"
cp "$EFI_STUB" "$ISO_STAGE/boot/nexsos-efi-loader.efi"
cat > "$ISO_STAGE/boot/grub/grub.cfg" <<'GRUB_CONFIG'
echo "NEXSOS_GRUB_ARM64_CHAINLOAD_STUB"
chainloader /boot/nexsos-efi-loader.efi
boot
echo "NEXSOS_EFI_STUB_RETURNED"
GRUB_CONFIG

xorriso -as mkisofs -R -J -V NEXSAA64 \
    -o "$ISO" -e boot/grub/efi.img -no-emul-boot "$ISO_STAGE" \
    > "$LOG_DIR/xorriso-aarch64-uefi.log" 2>&1

xorriso -indev "$ISO" -report_el_torito plain \
    > "$LOG_DIR/aarch64-uefi-el-torito.txt" 2>&1
grep -Eiq 'Platform Id: *0xEF|UEFI' "$LOG_DIR/aarch64-uefi-el-torito.txt" || {
    echo "[aarch64-uefi] ISO has no UEFI El Torito boot entry" >&2
    cat "$LOG_DIR/aarch64-uefi-el-torito.txt" >&2
    exit 1
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}
{
    echo "architecture=aarch64"
    echo "bootloader=GNU GRUB ${NEXS_GRUB_VERSION:-2.14} (arm64-efi)"
    echo "efi_stub=$EFI_STUB"
    echo "kernel_sha256=$(sha256_file "$KERNEL")"
    echo "dtb_sha256=$(sha256_file "$DTB")"
    echo "efi_stub_sha256=$(sha256_file "$EFI_STUB")"
    echo "iso_sha256=$(sha256_file "$ISO")"
} > "$EFI_ROOT/manifest.txt"

echo "[aarch64-uefi] bootable ISO ready: $ISO"
