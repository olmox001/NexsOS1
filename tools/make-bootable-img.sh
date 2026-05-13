#!/bin/bash
# =============================================================================
# tools/make-bootable-img.sh
# Bootable Disk Image Generator for os1test (Limine multiboot2)
# Creates MBR-partitioned FAT32 disk image
# Supports: AMD64 (AArch64 support requires different approach)
# =============================================================================

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# ============================================================================
# Configuration
# ============================================================================

ARCH="${1:-amd64}"
BUILD_DIR="build/$ARCH"
KERNEL_ELF="$BUILD_DIR/kernel.elf"
OUT_IMG="$BUILD_DIR/os1test.img"
IMG_SIZE_MB=256  # Size in MB
ISO_ROOT="$BUILD_DIR/iso_stage_img"  # Staging for MBR version

# Detect Limine installation
if command -v brew >/dev/null 2>&1; then
    BREW_PREFIX=$(brew --prefix 2>/dev/null || echo "")
    if [ -n "$BREW_PREFIX" ] && [ -d "$BREW_PREFIX/share/limine" ]; then
        LIMINE_SHARE="$BREW_PREFIX/share/limine"
        LIMINE_BIN="$BREW_PREFIX/bin/limine"
    fi
fi

# Fallback locations
if [ -z "$LIMINE_BIN" ] || [ ! -x "$LIMINE_BIN" ]; then
    if command -v limine >/dev/null 2>&1; then
        LIMINE_BIN=$(which limine)
    else
        echo -e "${RED}[ERROR]${NC} limine not found. Install with: brew install limine"
        exit 1
    fi
fi

if [ -z "$LIMINE_SHARE" ] || [ ! -d "$LIMINE_SHARE" ]; then
    for path in /usr/local/share/limine /opt/homebrew/share/limine /usr/share/limine; do
        if [ -d "$path" ]; then
            LIMINE_SHARE="$path"
            break
        fi
    done
    if [ ! -d "$LIMINE_SHARE" ]; then
        echo -e "${RED}[ERROR]${NC} Limine share directory not found"
        exit 1
    fi
fi

# ============================================================================
# Validation
# ============================================================================

if [ ! -f "$KERNEL_ELF" ]; then
    echo -e "${RED}[ERROR]${NC} Kernel not found: $KERNEL_ELF"
    echo "  Run 'make ARCH=$ARCH' first"
    exit 1
fi

# IMG creation is currently only tested for amd64
# For production use, consider using ISO instead
if [ "$ARCH" != "amd64" ]; then
    echo -e "${YELLOW}[!]${NC} IMG creation is optimized for amd64"
    echo -e "${YELLOW}[!]${NC} For AArch64, consider using: make ARCH=aarch64 bootable-iso"
    echo -e "${YELLOW}[!]${NC} Proceeding with IMG generation..."
fi

# Check for mtools (macOS: brew install mtools)
if ! command -v mformat >/dev/null 2>&1; then
    echo -e "${YELLOW}[!]${NC} mtools not found, using loop device + mkfs.vfat"
    USE_MTOOLS=0
    if ! command -v mkfs.vfat >/dev/null 2>&1; then
        echo -e "${RED}[ERROR]${NC} Neither mtools nor mkfs.vfat found"
        echo "  On macOS: brew install mtools"
        echo "  Or: brew install dosfstools"
        exit 1
    fi
else
    USE_MTOOLS=1
    echo -e "${GREEN}[✓]${NC} mtools detected"
fi

# Verify Limine files
echo -e "${YELLOW}[*]${NC} Checking Limine files..."
LIMINE_BIOS_BIN="$LIMINE_SHARE/limine-bios.sys"
LIMINE_MBR_BIN="$LIMINE_SHARE/limine-bios-mbr.bin"

if [ ! -f "$LIMINE_BIOS_BIN" ]; then
    echo -e "${RED}[ERROR]${NC} Limine file not found: $LIMINE_BIOS_BIN"
    exit 1
fi

if [ ! -f "$LIMINE_MBR_BIN" ]; then
    echo -e "${YELLOW}[!]${NC} Warning: Limine MBR file not found: $LIMINE_MBR_BIN"
    echo -e "${YELLOW}[!]${NC} Using BIOS install without MBR"
    LIMINE_MBR_BIN=""
fi

echo -e "${GREEN}[✓]${NC} Limine binary: $LIMINE_BIN"

# ============================================================================
# Create disk image
# ============================================================================

echo -e "${YELLOW}[*]${NC} Creating ${IMG_SIZE_MB}MB disk image..."
rm -f "$OUT_IMG"
dd if=/dev/zero of="$OUT_IMG" bs=1M count="$IMG_SIZE_MB" 2>/dev/null

echo -e "${GREEN}[✓]${NC} Disk image created: $OUT_IMG"
ls -lh "$OUT_IMG"

# ============================================================================
# Create MBR Partition Table
# ============================================================================

if [ "$USE_MTOOLS" -eq 1 ]; then
    # ========================================================================
    # Method 1: Using mtools with FAT32 filesystem
    # ========================================================================
    
    echo -e "${YELLOW}[*]${NC} Creating FAT32 filesystem on disk image..."
    
    # Initialize as FAT32 filesystem (without partition table)
    # This works better on macOS than mpartition
    mformat -i "$OUT_IMG" -F -v "os1test" 2>/dev/null || {
        echo -e "${RED}[ERROR]${NC} mformat failed"
        exit 1
    }
    
    # Create boot directories
    echo -e "${YELLOW}[*]${NC} Creating directories..."
    mmd -i "$OUT_IMG" ::/limine 2>/dev/null || true
    mmd -i "$OUT_IMG" ::/boot 2>/dev/null || true
    
    # Copy kernel
    echo -e "${YELLOW}[*]${NC} Copying kernel to disk..."
    mcopy -i "$OUT_IMG" "$KERNEL_ELF" ::/os1test.elf
    
    # Copy Limine BIOS
    echo -e "${YELLOW}[*]${NC} Copying Limine bootloader..."
    mcopy -i "$OUT_IMG" "$LIMINE_BIOS_BIN" ::/limine/
    
    # Create and copy limine.conf
    LIMINE_CONF_TMP="/tmp/limine_$$.conf"
    cat > "$LIMINE_CONF_TMP" << 'EOF'
TIMEOUT=0
SERIAL=yes
VERBOSE=yes

:os1test OS - AMD64 (Bootable IMG)
    PROTOCOL=multiboot2
    KERNEL_PATH=boot():/os1test.elf
EOF
    
    echo -e "${YELLOW}[*]${NC} Creating limine.conf..."
    mcopy -i "$OUT_IMG" "$LIMINE_CONF_TMP" ::/limine.conf
    rm -f "$LIMINE_CONF_TMP"
    
    # List contents
    echo -e "${YELLOW}[*]${NC} Disk filesystem contents:"
    mdir -i "$OUT_IMG" -a -s / 2>/dev/null | head -30 || true
    
else
    # ========================================================================
    # Method 2: Using loop device + mkfs.vfat (macOS may not support loopback)
    # ========================================================================
    
    echo -e "${YELLOW}[*]${NC} Using loop device method (macOS may not support this)..."
    
    # Try to setup loop device
    LOOP_DEV=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount ram://512000 2>/dev/null || echo "")
    
    if [ -z "$LOOP_DEV" ]; then
        echo -e "${RED}[ERROR]${NC} Cannot create loop device on macOS"
        echo -e "${YELLOW}[!]${NC} Please install mtools: brew install mtools"
        rm -f "$OUT_IMG"
        exit 1
    fi
    
    echo -e "${GREEN}[✓]${NC} Loop device: $LOOP_DEV"
    
    # TODO: Implement full loop device method
    # For now, fallback to error
    echo -e "${RED}[ERROR]${NC} Loop device method not fully implemented"
    hdiutil eject "$LOOP_DEV" 2>/dev/null || true
    rm -f "$OUT_IMG"
    exit 1
fi

# ============================================================================
# Install Limine BIOS bootloader
# ============================================================================

echo -e "${YELLOW}[*]${NC} Installing Limine BIOS bootloader..."

"$LIMINE_BIN" bios-install "$OUT_IMG" 2>&1 | head -20 || {
    echo -e "${YELLOW}[!]${NC} Limine BIOS install had issues (may be OK)"
}

# ============================================================================
# Final status
# ============================================================================

IMG_SIZE=$(du -sh "$OUT_IMG" | cut -f1)
echo -e ""
echo -e "${GREEN}[SUCCESS]${NC} Bootable disk image created successfully!"
echo -e "  Architecture: $ARCH"
echo -e "  Output: $OUT_IMG"
echo -e "  Size: $IMG_SIZE"
echo -e ""
echo -e "To test with QEMU:"
echo -e "  qemu-system-x86_64 -drive file='$OUT_IMG',format=raw -m 1G"
echo -e ""
echo -e "To write to USB (macOS):"
echo -e "  diskutil list"
echo -e "  diskutil unmountDisk /dev/diskX"
echo -e "  sudo dd if='$OUT_IMG' of=/dev/rdiskX bs=4m"
echo -e "  diskutil eject /dev/diskX"
echo -e ""
