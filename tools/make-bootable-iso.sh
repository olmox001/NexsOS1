#!/bin/bash
# =============================================================================
# tools/make-bootable-iso.sh
# Bootable ISO Generator for NexsOS1 (Limine multiboot2)
# Supports: AMD64 and AArch64
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
OUT_ISO="$BUILD_DIR/NexsOS1.iso"
ISO_ROOT="$BUILD_DIR/iso_stage"

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

# Limine share directory fallback
if [ -z "$LIMINE_SHARE" ] || [ ! -d "$LIMINE_SHARE" ]; then
    # Try common paths
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

if ! command -v xorriso >/dev/null 2>&1; then
    echo -e "${RED}[ERROR]${NC} xorriso not found. Install with: brew install xorriso"
    exit 1
fi

# ============================================================================
# Verify Limine files
# ============================================================================

echo -e "${YELLOW}[*]${NC} Checking Limine files..."

case "$ARCH" in
    amd64)
        LIMINE_BIOS_BIN="$LIMINE_SHARE/limine-bios.sys"
        LIMINE_BIOS_CD="$LIMINE_SHARE/limine-bios-cd.bin"
        
        if [ ! -f "$LIMINE_BIOS_BIN" ] || [ ! -f "$LIMINE_BIOS_CD" ]; then
            echo -e "${RED}[ERROR]${NC} Limine BIOS files not found"
            echo "  Looking for:"
            echo "    - $LIMINE_BIOS_BIN"
            echo "    - $LIMINE_BIOS_CD"
            exit 1
        fi
        
        LIMINE_CD_LOADER="$LIMINE_BIOS_CD"
        ;;
    aarch64)
        LIMINE_UEFI_CD="$LIMINE_SHARE/limine-uefi-cd.bin"
        
        if [ ! -f "$LIMINE_UEFI_CD" ]; then
            echo -e "${RED}[ERROR]${NC} Limine UEFI-CD file not found: $LIMINE_UEFI_CD"
            exit 1
        fi
        
        LIMINE_CD_LOADER="$LIMINE_UEFI_CD"
        ;;
    *)
        echo -e "${RED}[ERROR]${NC} Unsupported architecture: $ARCH"
        exit 1
        ;;
esac

echo -e "${GREEN}[✓]${NC} Limine binary: $LIMINE_BIN"
echo -e "${GREEN}[✓]${NC} Limine CD loader: $LIMINE_CD_LOADER"

# ============================================================================
# Create ISO staging directory
# ============================================================================

echo -e "${YELLOW}[*]${NC} Preparing ISO staging directory..."
rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/limine"
mkdir -p "$ISO_ROOT/boot"

# ============================================================================
# Copy kernel
# ============================================================================

echo -e "${YELLOW}[*]${NC} Copying kernel..."
cp "$KERNEL_ELF" "$ISO_ROOT/NexsOS1.elf"
ls -lh "$ISO_ROOT/NexsOS1.elf"

# ============================================================================
# Copy Limine files (architecture-specific)
# ============================================================================

echo -e "${YELLOW}[*]${NC} Copying Limine bootloader files..."

case "$ARCH" in
    amd64)
        # Copy BIOS files for x86-64
        cp "$LIMINE_BIOS_BIN" "$ISO_ROOT/limine/"
        cp "$LIMINE_BIOS_CD" "$ISO_ROOT/limine/"
        ls -lh "$ISO_ROOT/limine/"
        ;;
    aarch64)
        # For UEFI, BOOTAA64.EFI is needed for EFI boot
        BOOTAA64="$LIMINE_SHARE/BOOTAA64.EFI"
        if [ -f "$BOOTAA64" ]; then
            mkdir -p "$ISO_ROOT/EFI/BOOT"
            cp "$BOOTAA64" "$ISO_ROOT/EFI/BOOT/"
        fi
        cp "$LIMINE_UEFI_CD" "$ISO_ROOT/limine/"
        ls -lh "$ISO_ROOT/limine/" "$ISO_ROOT/EFI/BOOT/" 2>/dev/null || true
        ;;
esac

# ============================================================================
# Create limine.conf
# ============================================================================

echo -e "${YELLOW}[*]${NC} Generating limine.conf..."

LIMINE_CONF="$ISO_ROOT/limine.conf"

case "$ARCH" in
    amd64)
        cat > "$LIMINE_CONF" << 'EOF'
TIMEOUT=0
SERIAL=yes
VERBOSE=yes

:NexsOS1 OS - AMD64
    PROTOCOL=multiboot2
    KERNEL_PATH=boot():/NexsOS1.elf
EOF
        ;;
    aarch64)
        cat > "$LIMINE_CONF" << 'EOF'
TIMEOUT=0
SERIAL=yes
VERBOSE=yes

:NexsOS1 OS - AArch64
    PROTOCOL=limine
    KERNEL_PATH=boot():/NexsOS1.elf
EOF
        ;;
    *)
        echo -e "${RED}[ERROR]${NC} Unsupported architecture: $ARCH"
        exit 1
        ;;
esac

cat "$LIMINE_CONF"

# ============================================================================
# Create ISO
# ============================================================================

echo -e "${YELLOW}[*]${NC} Creating ISO with xorriso..."

case "$ARCH" in
    amd64)
        # BIOS boot for x86-64
        xorriso -as mkisofs \
            -R -J -V "NexsOS1_boot" \
            -b limine/limine-bios-cd.bin \
            -no-emul-boot \
            -boot-load-size 4 \
            -boot-info-table \
            -o "$OUT_ISO" \
            "$ISO_ROOT"
        ;;
    aarch64)
        # UEFI boot for AArch64
        xorriso -as mkisofs \
            -R -J -V "NexsOS1_boot" \
            -b limine/limine-uefi-cd.bin \
            -no-emul-boot \
            -boot-load-size 4 \
            -efi-boot limine/limine-uefi-cd.bin \
            -o "$OUT_ISO" \
            "$ISO_ROOT"
        ;;
esac

if [ ! -f "$OUT_ISO" ]; then
    echo -e "${RED}[ERROR]${NC} ISO creation failed"
    exit 1
fi

echo -e "${GREEN}[✓]${NC} ISO created: $OUT_ISO"
ls -lh "$OUT_ISO"

# ============================================================================
# Install Limine BIOS bootloader (AMD64 only)
# ============================================================================

if [ "$ARCH" = "amd64" ]; then
    echo -e "${YELLOW}[*]${NC} Installing Limine BIOS bootloader..."
    
    "$LIMINE_BIN" bios-install "$OUT_ISO"
    
    if [ $? -ne 0 ]; then
        echo -e "${YELLOW}[!]${NC} Limine BIOS install returned non-zero (may be OK on macOS)"
    fi
else
    echo -e "${YELLOW}[*]${NC} AArch64 UEFI boot - no BIOS install needed"
fi

# ============================================================================
# Verify ISO content
# ============================================================================

echo -e "${YELLOW}[*]${NC} ISO content:"
xorriso -indev "$OUT_ISO" -ls / 2>/dev/null | head -20 || true

# ============================================================================
# Final status
# ============================================================================

ISO_SIZE=$(du -sh "$OUT_ISO" | cut -f1)
echo -e ""
echo -e "${GREEN}[SUCCESS]${NC} Bootable ISO created successfully!"
echo -e "  Architecture: $ARCH"
echo -e "  Output: $OUT_ISO"
echo -e "  Size: $ISO_SIZE"
echo -e ""
echo -e "To test with QEMU:"
case "$ARCH" in
    amd64)
        echo -e "  qemu-system-x86_64 -cdrom '$OUT_ISO' -boot d -m 1G"
        ;;
    aarch64)
        echo -e "  qemu-system-aarch64 -M virt -cdrom '$OUT_ISO' -m 2G"
        ;;
esac
echo -e ""
echo -e "Or use: make ARCH=$ARCH run-iso"
echo -e ""
