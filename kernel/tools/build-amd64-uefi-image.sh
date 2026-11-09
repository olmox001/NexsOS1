#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_ROOT="$(cd "$ROOT/.." && pwd)"
GRUB_MANIFEST="$PROJECT_ROOT/boot/grub/manifest.env"
# shellcheck disable=SC1090
source "$GRUB_MANIFEST"
BUILD_ROOT="${NEXS_BUILD_ROOT:-$PROJECT_ROOT/build}"
case "$BUILD_ROOT" in
    /*) ;;
    *) echo "[amd64-uefi] NEXS_BUILD_ROOT must be absolute: $BUILD_ROOT" >&2; exit 1 ;;
esac

BUILD="$BUILD_ROOT/amd64"
GRUB_STAGE="$BUILD_ROOT/cache/grub/prebuilt/x86_64-efi"
GRUB_MKIMAGE="$GRUB_STAGE/bin/grub-mkimage"
GRUB_MODULE_DIR="$GRUB_STAGE/lib/grub/x86_64-efi"
LOG_DIR="$BUILD_ROOT/logs"
EFI_ROOT="$BUILD/grub-uefi"
ISO_STAGE="$EFI_ROOT/iso"
EFI_IMAGE="$EFI_ROOT/efi.img"
EFI_LOADER="$EFI_ROOT/EFI/BOOT/BOOTX64.EFI"
ISO="$EFI_ROOT/nexsos-amd64-uefi.iso"
KERNEL="$BUILD/kernel.elf"
GRUB_COMMIT="$NEXS_GRUB_COMMIT"

for tool in xorriso mformat mmd mcopy python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "[amd64-uefi] missing required tool: $tool" >&2
        exit 1
    }
done
[[ -s "$KERNEL" ]] || {
    echo "[amd64-uefi] missing kernel: $KERNEL (build kernel first)" >&2
    exit 1
}

env NEXS_BUILD_ROOT="$BUILD_ROOT" "$ROOT/tools/build-grub-x86_64.sh"
[[ -x "$GRUB_MKIMAGE" && -f "$GRUB_MODULE_DIR/multiboot2.mod" ]] || {
    echo "[amd64-uefi] reusable GRUB x86_64-efi stage is incomplete" >&2
    exit 1
}

case "$ISO_STAGE" in
    "$BUILD_ROOT"/amd64/grub-uefi/iso) ;;
    *) echo "[amd64-uefi] unsafe staging path: $ISO_STAGE" >&2; exit 1 ;;
esac
rm -rf "$ISO_STAGE"
mkdir -p "$ISO_STAGE/boot/grub" "$(dirname "$EFI_LOADER")" "$LOG_DIR"

cat > "$EFI_ROOT/embedded.cfg" <<'GRUB_EMBEDDED'
search --no-floppy --file --set=root /boot/grub/grub.cfg
configfile /boot/grub/grub.cfg
GRUB_EMBEDDED

"$GRUB_MKIMAGE" -O x86_64-efi     -d "$GRUB_MODULE_DIR"     -o "$EFI_LOADER"     -p /boot/grub -c "$EFI_ROOT/embedded.cfg"     normal multiboot2 iso9660 search search_fs_file configfile echo     fat part_gpt part_msdos serial help reboot halt

python3 - "$EFI_IMAGE" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
path.parent.mkdir(parents=True, exist_ok=True)
with path.open("wb") as stream:
    stream.truncate(8 * 1024 * 1024)
PY

mformat -i "$EFI_IMAGE" -v NEXSX64 ::
mmd -i "$EFI_IMAGE" ::/EFI
mmd -i "$EFI_IMAGE" ::/EFI/BOOT
mcopy -i "$EFI_IMAGE" "$EFI_LOADER" ::/EFI/BOOT/BOOTX64.EFI

cp "$EFI_IMAGE" "$ISO_STAGE/boot/grub/efi.img"
cp "$KERNEL" "$ISO_STAGE/boot/kernel.elf"
cat > "$ISO_STAGE/boot/grub/grub.cfg" <<'GRUB_CONFIG'
set timeout=0
set default=0
insmod serial
insmod multiboot2
serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1
terminal_input serial
terminal_output serial
echo "NEXSOS_GRUB_AMD64_UEFI_MULTIBOOT2"
multiboot2 /boot/kernel.elf
boot
GRUB_CONFIG

xorriso -as mkisofs -R -J -V NEXSX64     -o "$ISO" -e boot/grub/efi.img -no-emul-boot "$ISO_STAGE"     > "$LOG_DIR/xorriso-amd64-uefi.log" 2>&1
xorriso -indev "$ISO" -report_el_torito plain     > "$LOG_DIR/amd64-uefi-el-torito.txt" 2>&1
grep -Eiq 'Platform Id: *0xEF|UEFI' "$LOG_DIR/amd64-uefi-el-torito.txt" || {
    echo "[amd64-uefi] ISO has no UEFI El Torito boot entry" >&2
    cat "$LOG_DIR/amd64-uefi-el-torito.txt" >&2
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
    echo "architecture=amd64"
    echo "firmware=UEFI"
    echo "bootloader=GNU GRUB 2.14 (x86_64-efi)"
    echo "grub_commit=$GRUB_COMMIT"
    echo "kernel_sha256=$(sha256_file "$KERNEL")"
    echo "efi_loader_sha256=$(sha256_file "$EFI_LOADER")"
    echo "iso_sha256=$(sha256_file "$ISO")"
} > "$EFI_ROOT/manifest.txt"

echo "[amd64-uefi] bootable ISO ready: $ISO"
