#!/usr/bin/env bash
#
# tools/setup-toolchain-linux.sh
# ------------------------------------------------------------------------------
# Install the EXACT NexsOS1 / NEXS dual-arch cross toolchain on Linux,
# matching the macOS environment as closely as possible.
#
#   amd64    : x86_64-elf-binutils + x86_64-elf-gcc  (built from GNU source)
#              -> prefix x86_64-elf-      (same as macOS Makefile)
#   aarch64  : aarch64-linux-gnu-gcc                  (distro package)
#              -> prefix aarch64-linux-gnu- (Makefile.linux)
#   emulator : qemu-system-x86_64 + qemu-system-aarch64
#   disk     : xorriso, grub-pc-bin, mtools
#
# Supported distros: Ubuntu/Debian, Arch Linux, Alpine Linux.
#
# The x86_64-elf cross-compiler is NOT available as an official distro package
# on any major Linux distribution.  This script builds it from the official GNU
# mirror (same sources Homebrew uses on macOS), installs it to /usr/local, and
# verifies the result.
#
# Idempotent: safe to re-run (skips build if x86_64-elf-gcc is already present
# and at the expected version).
# ------------------------------------------------------------------------------
set -euo pipefail

info()  { printf '\033[1;34m[toolchain]\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m[toolchain]\033[0m %s\n' "$*"; }
fail()  { printf '\033[1;31m[toolchain]\033[0m %s\n' "$*" >&2; exit 1; }

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ "$(id -u)" = "0" ]; then
    SUDO=()
else
    command -v sudo >/dev/null 2>&1 || fail "sudo not found; install sudo or run as root."
    SUDO=(sudo)
fi

init_submodules() {
    info "Initializing git submodules..."
    (cd "$REPO_ROOT" && git submodule sync --recursive) || warn "Failed to sync submodules."
    (cd "$REPO_ROOT" && git submodule update --init --recursive) || warn "Failed to initialize submodules."
}

# ==============================================================================
# Configuration — bump these to upgrade the cross toolchain
# ==============================================================================
BINUTILS_VERSION=2.42
GCC_VERSION=13.2.0
TARGET=x86_64-elf
PREFIX=/usr/local
JOBS=$(nproc 2>/dev/null || echo 4)

GNU_MIRROR=https://ftp.gnu.org/gnu

# ==============================================================================
# 0. Detect distro
# ==============================================================================
if [ -f /etc/os-release ]; then
    . /etc/os-release
    DISTRO=$ID
else
    fail "Cannot detect Linux distribution (no /etc/os-release)."
fi

info "Detected Linux distro: $DISTRO"

# ==============================================================================
# 1. Install distro packages (build deps, aarch64 cross-compiler, QEMU, etc.)
# ==============================================================================
install_packages() {
    case "$DISTRO" in
        ubuntu|debian|linuxmint|pop)
            info "Installing build dependencies and tools via apt..."
            "${SUDO[@]}" apt-get update
            "${SUDO[@]}" apt-get install -y \
                build-essential \
                file \
                bison flex \
                libgmp-dev libmpc-dev libmpfr-dev \
                texinfo \
                gcc-aarch64-linux-gnu \
                qemu-system-x86 \
                qemu-system-arm \
                grub-common grub-pc-bin \
                xorriso mtools \
                wget curl git
            ;;
        arch|manjaro|endeavouros)
            info "Installing build dependencies and tools via pacman..."
            "${SUDO[@]}" pacman -Syu --noconfirm --needed \
                base-devel \
                file \
                gmp libmpc mpfr \
                texinfo \
                aarch64-linux-gnu-gcc \
                qemu-system-x86 qemu-system-aarch64 \
                grub libisoburn mtools \
                wget git
            ;;
        alpine)
            info "Installing build dependencies and tools via apk..."
            "${SUDO[@]}" apk update
            "${SUDO[@]}" apk add \
                build-base \
                file \
                bison flex \
                gmp-dev mpc1-dev mpfr-dev \
                texinfo \
                qemu-system-x86_64 qemu-system-aarch64 \
                grub xorriso mtools \
                wget curl git bash
            warn "Alpine: aarch64 bare-metal cross-compiler may need manual setup."
            ;;
        *)
            warn "Distro '$DISTRO' not directly supported."
            warn "Please install manually: build-essential, bison, flex, libgmp-dev,"
            warn "libmpc-dev, libmpfr-dev, texinfo, gcc-aarch64-linux-gnu, qemu, grub, xorriso."
            ;;
    esac
}

# ==============================================================================
# 2. Build x86_64-elf cross-compiler from GNU sources
# ==============================================================================
build_cross_toolchain() {
    # Skip if already installed at the expected version
    if command -v ${TARGET}-gcc >/dev/null 2>&1; then
        local installed_ver
        installed_ver=$(${TARGET}-gcc --version 2>/dev/null | head -1 | grep -oP '\d+\.\d+\.\d+' || echo "unknown")
        if [ "$installed_ver" = "$GCC_VERSION" ]; then
            info "${TARGET}-gcc $GCC_VERSION already installed — skipping build."
            return 0
        else
            info "${TARGET}-gcc found ($installed_ver) but expected $GCC_VERSION — rebuilding."
        fi
    fi

    local BUILD_DIR
    BUILD_DIR=$(mktemp -d)
    info "Building ${TARGET} cross-compiler in $BUILD_DIR ..."
    info "  binutils: $BINUTILS_VERSION"
    info "  gcc:      $GCC_VERSION"
    info "  prefix:   $PREFIX"
    info "  jobs:     $JOBS"

    cd "$BUILD_DIR"

    # --- binutils ---
    info "Downloading binutils-${BINUTILS_VERSION}..."
    wget -q "${GNU_MIRROR}/binutils/binutils-${BINUTILS_VERSION}.tar.xz"
    tar xf "binutils-${BINUTILS_VERSION}.tar.xz"

    mkdir -p build-binutils && cd build-binutils
    info "Configuring binutils..."
    "../binutils-${BINUTILS_VERSION}/configure" \
        --target="${TARGET}" \
        --prefix="${PREFIX}" \
        --with-sysroot \
        --disable-nls \
        --disable-werror \
        --quiet
    info "Building binutils (${JOBS} jobs)..."
    make -j"${JOBS}" -s
    info "Installing binutils..."
    "${SUDO[@]}" make install -s
    cd "$BUILD_DIR"

    # --- gcc ---
    info "Downloading gcc-${GCC_VERSION}..."
    wget -q "${GNU_MIRROR}/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz"
    tar xf "gcc-${GCC_VERSION}.tar.xz"

    mkdir -p build-gcc && cd build-gcc
    info "Configuring gcc..."
    "../gcc-${GCC_VERSION}/configure" \
        --target="${TARGET}" \
        --prefix="${PREFIX}" \
        --disable-nls \
        --enable-languages=c,c++ \
        --without-headers \
        --disable-hosted-libstdcxx \
        --quiet
    info "Building gcc (${JOBS} jobs — this may take several minutes)..."
    make all-gcc -j"${JOBS}" -s
    make all-target-libgcc -j"${JOBS}" -s
    info "Installing gcc..."
    "${SUDO[@]}" make install-gcc -s
    "${SUDO[@]}" make install-target-libgcc -s
    cd "$BUILD_DIR"

    # Cleanup
    info "Cleaning up build directory..."
    rm -rf "$BUILD_DIR"
    info "${TARGET}-gcc ${GCC_VERSION} installed to ${PREFIX}."
}

# ==============================================================================
# 4. Verify
# ==============================================================================
verify_toolchain() {
    echo
    info "Installed versions:"
    ${TARGET}-gcc --version          2>/dev/null | head -1 || warn "${TARGET}-gcc missing!"
    ${TARGET}-ld  --version          2>/dev/null | head -1 || warn "${TARGET}-ld missing!"
    aarch64-linux-gnu-gcc --version  2>/dev/null | head -1 || warn "aarch64-linux-gnu-gcc missing!"
    qemu-system-x86_64  --version   2>/dev/null | head -1 || warn "qemu-system-x86_64 missing!"
    qemu-system-aarch64 --version   2>/dev/null | head -1 || warn "qemu-system-aarch64 missing!"

    # Sanity: aarch64 cross-compiler must NOT emit outline-atomics when -mno-outline-atomics is used
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        if echo 'int x;int f(void){return __atomic_add_fetch(&x,1,5);}' \
             | aarch64-linux-gnu-gcc -O2 -mno-outline-atomics -S -o - -xc - 2>/dev/null | grep -q '__aarch64_'; then
            warn "aarch64 gcc still emits outline-atomics even with -mno-outline-atomics! Check your GCC version."
        else
            info "aarch64 atomics inline with -mno-outline-atomics (OK)."
        fi
    fi

    echo
    info "make check ARCH=amd64:";   make -C "$REPO_ROOT" check ARCH=amd64   || true
    echo
    info "make check ARCH=aarch64:"; make -C "$REPO_ROOT" check ARCH=aarch64 || true
    echo
    info "Toolchain ready. Build:  make all ARCH=amd64   |   make all ARCH=aarch64"
}

# ==============================================================================
# Main
# ==============================================================================
[ "$(uname -s)" = "Linux" ] || fail "This script targets Linux only."

install_packages
init_submodules
build_cross_toolchain
verify_toolchain
