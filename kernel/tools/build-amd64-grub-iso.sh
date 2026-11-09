#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${NEXS_BUILD_ROOT:-$ROOT/build}"
case "$BUILD_ROOT" in
    /*) ;;
    *) echo "[amd64-grub] NEXS_BUILD_ROOT must be absolute: $BUILD_ROOT" >&2; exit 1 ;;
esac
BUILD="$BUILD_ROOT/amd64"
LOG_DIR="$BUILD_ROOT/logs"
STAGE="$BUILD/grub-bios"
ISO="$BUILD/grub-bios.iso"
KERNEL="$BUILD/kernel.elf"
GRUB_MKRESCUE="${NEXS_GRUB_MKRESCUE:-}"

if [[ -z "$GRUB_MKRESCUE" ]]; then
    if command -v i686-elf-grub-mkrescue >/dev/null 2>&1; then
        GRUB_MKRESCUE="$(command -v i686-elf-grub-mkrescue)"
    elif command -v grub-mkrescue >/dev/null 2>&1; then
        GRUB_MKRESCUE="$(command -v grub-mkrescue)"
    else
        echo "[amd64-grub] missing grub-mkrescue (i686-elf-grub-mkrescue on macOS)" >&2
        exit 1
    fi
fi
command -v xorriso >/dev/null 2>&1 || { echo "[amd64-grub] xorriso is required" >&2; exit 1; }
[[ -s "$KERNEL" ]] || { echo "[amd64-grub] missing kernel: $KERNEL (build kernel first)" >&2; exit 1; }

case "$STAGE" in "$BUILD_ROOT"/amd64/grub-bios) ;; *) echo "unsafe GRUB staging path: $STAGE" >&2; exit 1 ;; esac
rm -rf "$STAGE"
mkdir -p "$STAGE/boot/grub" "$LOG_DIR"
cp "$KERNEL" "$STAGE/boot/kernel.elf"
cat > "$STAGE/boot/grub/grub.cfg" <<'GRUB_CONFIG'
set timeout=0
set default=0
insmod serial
insmod multiboot2
serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1
terminal_input serial
terminal_output serial
menuentry "NexsOS1 AMD64" {
    multiboot2 /boot/kernel.elf
    boot
}
GRUB_CONFIG

"$GRUB_MKRESCUE" -o "$ISO" "$STAGE" \
    > "$LOG_DIR/grub-mkrescue-amd64.log" 2>&1
xorriso -indev "$ISO" -report_el_torito plain \
    > "$LOG_DIR/amd64-grub-el-torito.txt" 2>&1
grep -Eiq 'Platform Id: *0x00|BIOS' "$LOG_DIR/amd64-grub-el-torito.txt" || {
    echo "[amd64-grub] ISO has no BIOS El Torito boot entry" >&2
    cat "$LOG_DIR/amd64-grub-el-torito.txt" >&2
    exit 1
}
echo "[amd64-grub] bootable ISO ready: $ISO"
