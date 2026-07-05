#!/usr/bin/env bash
#
# tools/setup-toolchain-linux.sh
# ------------------------------------------------------------------------------
# Install the EXACT NexsOS1 / NEXS dual-arch cross toolchain on Linux,
# matching the macOS environment as closely as possible.
#
# amd64    : x86_64-elf-binutils + x86_64-elf-gcc/g++       (built from GNU source)
#            -> prefix x86_64-elf-      (same as macOS Makefile)
# aarch64  : aarch64-none-elf-binutils + aarch64-none-elf-gcc/g++ 7.2.0  (built from GNU source)
#            -> prefix aarch64-none-elf- (same as macOS Makefile / sergiobenitez/osxct tap)
#
# WHY GCC 7.2.0 for aarch64, and why NOT the distro aarch64-linux-gnu-gcc:
#   The kernel links -nostdlib (no libc) and boots a raw freestanding image.
#   GCC >= 10 (and Ubuntu's distro aarch64-linux-gnu-gcc, which is GCC 15.x)
#   default -moutline-atomics ON and use emulated TLS, emitting
#   __aarch64_*_sync / __emutls_get_address / abort that the freestanding
#   link cannot resolve — and even when forced to link, the resulting kernel
#   does NOT boot. This is true regardless of target triple (-linux-gnu or
#   -none-elf): it's the GCC *version* that matters, not the ABI suffix.
#   7.2.0 predates all of that and is the version the macOS side is pinned
#   to and verified against. Linux must use the exact same GCC version to
#   get the same behavior — a newer aarch64-none-elf would have the same
#   outline-atomics problem as aarch64-linux-gnu 15.x.
#
# emulator : qemu-system-x86_64 + qemu-system-aarch64
# disk     : xorriso, grub-pc-bin, mtools
# debug    : gdb-multiarch (Makefile's `debug` target uses -s -S gdbstub)
#
# Supported distros: Ubuntu/Debian, Arch Linux, Alpine Linux.
#
# NEITHER cross-compiler is available as a distro package on any major Linux
# distribution at these pinned versions. Both are built from the official GNU
# mirror (same sources Homebrew/osxct use on macOS), installed to /usr/local,
# and verified.
#
# NOTE ON BUILDING GCC 7.2.0 ON A MODERN HOST: this is a ~9 year old GCC
# release being compiled by whatever host GCC/glibc your distro ships today
# (e.g. Ubuntu 26 ships GCC 13/14+ and a very recent glibc). Old GCC source
# trees are known to hit host-side build failures on very new host toolchains
# (stricter default warnings promoted by the *host* compiler while building
# GCC's own sources, occasionally -Werror on things GCC 7's build system
# didn't anticipate). This script disables werror and doc generation
# defensively, but if the aarch64-none-elf build still fails, send me the
# actual configure/make error output (not just the exit code) and we patch
# it — do NOT assume the x86_64-elf build path will behave identically,
# since that one builds a modern GCC (13.2.0) with no such friction.
#
# Idempotent: safe to re-run (skips build if the target compiler is already
# present at the expected version).
# ------------------------------------------------------------------------------
set -euo pipefail

info() { printf '\033[1;34m[toolchain]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[toolchain]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[toolchain]\033[0m %s\n' "$*" >&2; exit 1; }

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
# Overridable: some distros/users prefer ~/.local or /opt over sudo-writing
# into /usr/local. Left at /usr/local by default to match the Makefile's
# assumption that cross-gcc is already on PATH without extra configuration.
PREFIX=${PREFIX:-/usr/local}
JOBS=$(nproc 2>/dev/null || echo 4)
GNU_MIRROR=https://ftp.gnu.org/gnu

# aarch64 side — pinned to match the macOS sergiobenitez/osxct tap exactly.
# BEST-EFFORT binutils pairing for GCC 7.2.0 (crosstool-ng-era recipes commonly
# pair 7.2.0 with binutils in the 2.28-2.30 range). If you have access to the
# Mac, confirm the exact binutils version with `aarch64-none-elf-ld --version`
# and change AARCH64_BINUTILS_VERSION below to match precisely — ld/as version
# skew is far less likely to break freestanding boot than the GCC version, but
# for a "byte for byte" match it's worth checking.
AARCH64_BINUTILS_VERSION=2.30
AARCH64_GCC_VERSION=7.2.0
AARCH64_TARGET=aarch64-none-elf

# ==============================================================================
# 0. Detect distro
# ==============================================================================
if [ -f /etc/os-release ]; then
    . /etc/os-release
    DISTRO=$ID
    DISTRO_LIKE="${ID_LIKE:-}"
else
    fail "Cannot detect Linux distribution (no /etc/os-release)."
fi
info "Detected Linux distro: $DISTRO${DISTRO_LIKE:+ (like: $DISTRO_LIKE)}"

# Map unlisted derivatives onto the family whose package manager/toolchain
# path they actually inherit, via /etc/os-release's ID_LIKE (e.g. Kali,
# Raspbian, Zorin, Devuan -> debian; CachyOS, Garuda, ArcoLinux -> arch).
# Falls through to the '*' branch (manual instructions) if ID_LIKE doesn't
# match anything known either — we do NOT guess a package manager blindly.
case "$DISTRO" in
    ubuntu|debian|linuxmint|pop|arch|manjaro|endeavouros|alpine) ;; # already handled explicitly
    *)
        case " $DISTRO_LIKE " in
            *" debian "*|*" ubuntu "*)
                info "'$DISTRO' is Debian-derived (ID_LIKE=$DISTRO_LIKE) — treating as debian."
                DISTRO=debian
                ;;
            *" arch "*)
                info "'$DISTRO' is Arch-derived (ID_LIKE=$DISTRO_LIKE) — treating as arch."
                DISTRO=arch
                ;;
        esac
        ;;
esac

# WSL2 note: WSLg (Windows 11, or `wsl --update` on Windows 10 21H2+) provides
# the X11/Wayland socket automatically, so QEMU's default GTK display (see
# QEMU_FLAGS -display default in the Makefile) shows up as a normal window —
# no code patch needed for that part. What WSL2 does NOT guarantee is
# /dev/kvm: nested virtualization must be enabled Windows-side
# (.wslconfig -> [wsl2] nestedVirtualization=true, Windows 11 + a CPU/hypervisor
# that supports it) and even then is not always present. This project's
# QEMU_FLAGS already omit -enable-kvm / -accel kvm (runs on pure TCG software
# emulation), so nothing here depends on /dev/kvm — WSL2 works out of the box,
# just slower than bare-metal Linux.
if grep -qi microsoft /proc/version 2>/dev/null; then
    info "Running under WSL2."
    if [ ! -e /dev/kvm ]; then
        info "/dev/kvm not present — irrelevant here, this project's QEMU_FLAGS never request KVM acceleration (TCG only)."
    fi
    if [ -z "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
        warn "Neither WAYLAND_DISPLAY nor DISPLAY is set — WSLg may not be active."
        warn "Run 'wsl --update' from Windows PowerShell and restart the WSL2 distro, or QEMU's graphical window will not appear."
    else
        info "WSLg display detected (DISPLAY=${DISPLAY:-} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-}) — QEMU's window should show up normally."
    fi
fi

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
                libisl-dev libzstd-dev \
                texinfo \
                qemu-system-x86 \
                qemu-system-arm \
                grub-common grub-pc-bin \
                xorriso mtools \
                gdb-multiarch \
                wget curl git
            ;;
        arch|manjaro|endeavouros)
			info "Installing build dependencies and tools via pacman..."

			read -rp "Run a full system upgrade (pacman -Syu)? [y/N] " reply
			if [[ "$reply" =~ ^[Yy]$ ]]; then
				"${SUDO[@]}" pacman -Syu
			fi

			echo
			info "The following packages will be installed if missing:"
			cat <<EOF
base-devel
file
gmp libmpc mpfr isl zstd
texinfo
qemu-system-x86 qemu-system-aarch64
grub libisoburn mtools
gdb
wget git
EOF
			echo

			read -rp "Install these packages? [Y/n] " reply
			if [[ ! "$reply" =~ ^[Nn]$ ]]; then
				"${SUDO[@]}" pacman -S --needed \
					base-devel \
					file \
					gmp libmpc mpfr isl zstd \
					texinfo \
					qemu-system-x86 qemu-system-aarch64 \
					grub libisoburn mtools \
					gdb \
					wget git
			else
				warn "Package installation skipped."
				return 1
			fi
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
                grep \
                qemu-system-x86_64 qemu-system-aarch64 \
                grub xorriso mtools \
                gdb \
                wget curl git bash
            # NOTE: Alpine's default /bin/grep is busybox grep, which does NOT
            # support -oP (PCRE). The version-check below in
            # build_cross_toolchain() relies on `grep -oP`, so the GNU `grep`
            # package above is mandatory, not cosmetic.
            ;;
        *)
            warn "Distro '$DISTRO' not directly supported."
            warn "Please install manually: build-essential, bison, flex, libgmp-dev,"
            warn "libmpc-dev, libmpfr-dev, libisl-dev, libzstd-dev, texinfo, qemu, grub, xorriso."
            warn "Both cross-compilers (x86_64-elf and aarch64-none-elf) are built from"
            warn "source by this script — no distro cross-gcc package is required."
            ;;
    esac
}

build_gnu_cross_toolchain() {
    # $1=target-triple  $2=binutils-version  $3=gcc-version  $4=extra gcc configure flags (word-split)
    local t="$1" bv="$2" gv="$3"
    local extra_gcc_flags="$4"

    if command -v "${t}-gcc" >/dev/null 2>&1; then
        local installed_ver
        installed_ver=$("${t}-gcc" --version 2>/dev/null | head -1 | grep -oP '\d+\.\d+\.\d+' || echo "unknown")
        if [ "$installed_ver" = "$gv" ]; then
            info "${t}-gcc $gv already installed — skipping build."
            return 0
        else
            info "${t}-gcc found ($installed_ver) but expected $gv — rebuilding."
        fi
    fi

    # ------------------------------------------------------------------
    # Pick a scratch directory that is NOT on a tmpfs (see header note:
    # system /tmp is very commonly RAM-backed and small; building
    # binutils+gcc needs 3-6 GB and will spuriously EDQUOT there even
    # when the real disk has plenty of space).
    # ------------------------------------------------------------------
    local SCRATCH_ROOT
    if [ -n "${TMPDIR:-}" ]; then
        SCRATCH_ROOT="$TMPDIR"
    else
        SCRATCH_ROOT="$REPO_ROOT/.toolchain-build-tmp"
        mkdir -p "$SCRATCH_ROOT"
    fi
    local scratch_fstype
    scratch_fstype=$(df -T "$SCRATCH_ROOT" 2>/dev/null | tail -1 | awk '{print $2}')
    if [ "$scratch_fstype" = "tmpfs" ]; then
        warn "Scratch dir '$SCRATCH_ROOT' is on tmpfs (RAM-backed) — falling back to \$REPO_ROOT."
        SCRATCH_ROOT="$REPO_ROOT/.toolchain-build-tmp"
        mkdir -p "$SCRATCH_ROOT"
    fi
    local avail_mb
    avail_mb=$(df -Pm "$SCRATCH_ROOT" 2>/dev/null | tail -1 | awk '{print $4}')
    if [ -n "$avail_mb" ] && [ "$avail_mb" -lt 6144 ]; then
        warn "Only ${avail_mb}MB free on $(df -P "$SCRATCH_ROOT" | tail -1 | awk '{print $6}')."
        warn "binutils+gcc build typically needs 5-6GB of scratch space; this may fail."
    fi

    local BUILD_DIR
    BUILD_DIR=$(mktemp -d -p "$SCRATCH_ROOT")
    info "Building ${t} cross-compiler in $BUILD_DIR ..."
    info "  binutils: $bv"
    info "  gcc:      $gv"
    info "  prefix:   $PREFIX"
    info "  jobs:     $JOBS"

    (
        cd "$BUILD_DIR"

        # --- binutils ---
        info "Downloading binutils-${bv}..."
        wget -q "${GNU_MIRROR}/binutils/binutils-${bv}.tar.xz"
        tar xf "binutils-${bv}.tar.xz"
        mkdir -p build-binutils && cd build-binutils
        info "Configuring binutils..."
        "../binutils-${bv}/configure" \
            --target="${t}" \
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
        info "Downloading gcc-${gv}..."
        wget -q "${GNU_MIRROR}/gcc/gcc-${gv}/gcc-${gv}.tar.xz"
        tar xf "gcc-${gv}.tar.xz"
        mkdir -p build-gcc && cd build-gcc
        info "Configuring gcc..."
        # shellcheck disable=SC2086
        "../gcc-${gv}/configure" \
            --target="${t}" \
            --prefix="${PREFIX}" \
            --disable-nls \
            --disable-werror \
            --enable-languages=c,c++ \
            --without-headers \
            --disable-shared \
            --disable-threads \
            --disable-libssp \
            --disable-libquadmath \
            --disable-libgomp \
            $extra_gcc_flags \
            --quiet
        info "Building gcc (${JOBS} jobs — a from-source GCC build commonly takes 10-30 minutes, longer on slow/low-core VMs)..."
        # MAKEINFO=true: skip texinfo doc generation. Old GCC release tarballs
        # ship docs pre-built, but their Makefiles still probe for makeinfo and
        # can choke on a much newer makeinfo's stricter parser (irrelevant to
        # whether the compiler itself builds/works).
        ALLGCC_LOG=$(mktemp)
        if ! make MAKEINFO=true all-gcc -j"${JOBS}" -s > "$ALLGCC_LOG" 2>&1; then
            cat "$ALLGCC_LOG"
            if grep -qE "isl_(space|id)_(dim|free|get_user|alloc|get_tuple_id|range)" "$ALLGCC_LOG"; then
                echo
                fail "gcc-${gv} all-gcc failed on graphite/ISL API mismatch: this GCC's" \
                     "graphite pass targets an old libisl ABI that your system's isl-dev" \
                     "(too new) no longer exports. --without-isl should have disabled" \
                     "graphite entirely for this build — if you still hit this, the" \
                     "toolchain script you ran predates that fix: re-sync tools/setup-toolchain-linux.sh" \
                     "from the repo (this VM's copy is stale) and re-run. Do not try to" \
                     "'fix' this by installing an older libisl system-wide — it would" \
                     "collide with the host toolchain's own isl dependency."
            fi
            rm -f "$ALLGCC_LOG"
            fail "gcc-${gv} all-gcc build failed (see output above)."
        fi
        rm -f "$ALLGCC_LOG"
        make MAKEINFO=true all-target-libgcc -j"${JOBS}" -s
        info "Installing gcc..."
        "${SUDO[@]}" make MAKEINFO=true install-gcc -s
        "${SUDO[@]}" make MAKEINFO=true install-target-libgcc -s
    )
    local build_status=$?

    # Cleanup — always run from $REPO_ROOT, never from inside $BUILD_DIR
    # (avoids the "getcwd: No such file or directory" class of bug from
    # rm -rf'ing the shell's own current directory).
    cd "$REPO_ROOT"
    if [ "$build_status" -ne 0 ]; then
        fail "Build of ${t}-gcc ${gv} failed (see output above for the real configure/make error). Build dir kept at: $BUILD_DIR"
    fi

    info "Cleaning up build directory..."
    rm -rf "$BUILD_DIR"
    rmdir --ignore-fail-on-non-empty "$SCRATCH_ROOT" 2>/dev/null || true
    info "${t}-gcc ${gv} installed to ${PREFIX}."

    # Invalidate this shell's PATH lookup cache: a shell that already probed
    # for the compiler before install (e.g. the `command -v` check above) can
    # keep reporting "not found" until a new shell starts otherwise.
    hash -r 2>/dev/null || true
}

build_cross_toolchain() {
    # --disable-multilib: this target only ever needs the single ABI the
    # kernel/user ELFs are built for; multilib just multiplies libgcc build
    # time (and failure surface) for 32-bit/x32 variants nothing here uses.
    build_gnu_cross_toolchain "$TARGET" "$BINUTILS_VERSION" "$GCC_VERSION" "--disable-hosted-libstdcxx --disable-multilib"
}

build_aarch64_none_elf_toolchain() {
    # --with-newlib: tells GCC's build there is no target C library to assume
    # (matches the freestanding/-nostdlib kernel link). No -mno-outline-atomics
    # workaround needed here: GCC 7.2.0 predates -moutline-atomics entirely, so
    # passing that flag to it would itself be a hard configure/compile error —
    # the version pin IS the fix, not a compiler flag.
    #
    # --without-isl: GCC 7.2.0's graphite pass calls an old libisl API
    # (isl_space_dim, isl_space_free, ...) that current distro libisl-dev
    # (e.g. Ubuntu's 0.27) no longer exports — a genuine ISL ABI break, not a
    # -Werror artifact, and it fails all-gcc with "was not declared in this
    # scope". Graphite is a host-side loop optimization irrelevant to a
    # freestanding kernel cross-compiler, so disable it rather than pin/build
    # an ancient isl from source just to satisfy it.
    build_gnu_cross_toolchain "$AARCH64_TARGET" "$AARCH64_BINUTILS_VERSION" "$AARCH64_GCC_VERSION" "--with-newlib --without-isl --disable-multilib"
}

# ==============================================================================
# 4. Verify
# ==============================================================================
verify_toolchain() {
    echo
    info "Installed versions:"
    ${TARGET}-gcc --version 2>/dev/null | head -1 || warn "${TARGET}-gcc missing!"
    ${TARGET}-g++ --version 2>/dev/null | head -1 || warn "${TARGET}-g++ missing!"
    ${TARGET}-ld --version 2>/dev/null | head -1 || warn "${TARGET}-ld missing!"
    ${AARCH64_TARGET}-gcc --version 2>/dev/null | head -1 || warn "${AARCH64_TARGET}-gcc missing!"
    ${AARCH64_TARGET}-g++ --version 2>/dev/null | head -1 || warn "${AARCH64_TARGET}-g++ missing! (needed for kernel/drivers/cpp_test.cpp on aarch64)"
    ${AARCH64_TARGET}-ld --version 2>/dev/null | head -1 || warn "${AARCH64_TARGET}-ld missing!"
    qemu-system-x86_64 --version 2>/dev/null | head -1 || warn "qemu-system-x86_64 missing!"
    qemu-system-aarch64 --version 2>/dev/null | head -1 || warn "qemu-system-aarch64 missing!"

    # Sanity: this GCC must NOT emit outline-atomics helper calls at all.
    # We deliberately do NOT pass -mno-outline-atomics here: GCC 7.2.0
    # predates that flag entirely, and passing an unrecognized option would
    # itself be a build error, masking the real signal. The correct check for
    # a 7.2.0-era compiler is simply "does it ever emit __aarch64_* atomics
    # helpers" — it shouldn't, because the codegen feature doesn't exist yet.
    if command -v ${AARCH64_TARGET}-gcc >/dev/null 2>&1; then
        if echo 'int x;int f(void){return __atomic_add_fetch(&x,1,5);}' \
             | ${AARCH64_TARGET}-gcc -O2 -S -o - -xc - 2>/dev/null | grep -q '__aarch64_'; then
            warn "${AARCH64_TARGET}-gcc emits outline-atomics helpers — this is NOT GCC 7.2.0 behavior."
            warn "Check the installed version: ${AARCH64_TARGET}-gcc --version"
        else
            info "aarch64 atomics inline, no outline-atomics helpers (matches GCC 7.2.0 / macOS)."
        fi
    fi

    # A shell launched from a snap-packaged GUI app (VS Code's integrated
    # terminal is the common Ubuntu case) exports GTK_PATH/GTK_EXE_PREFIX/
    # GTK_MODULES/GIO_MODULE_DIR/XDG_DATA_DIRS/LOCPATH pointing into that
    # snap's own bundle (e.g. /snap/code/<rev>/usr/lib/.../gtk-3.0). Any GTK
    # app started from that shell — including QEMU with -display default,
    # which is a GTK frontend — inherits them, tries to dlopen its GTK
    # modules from the wrong snap's tree, and pulls in an old bundled
    # libpthread.so.0 that is ABI-incompatible with the host's glibc:
    # "undefined symbol __libc_pthread_init, version GLIBC_PRIVATE", and QEMU
    # dies before opening a window. Detect it here so it's diagnosed at setup
    # time instead of as a mystifying QEMU crash. The Makefile's QEMU_RUN
    # wrapper already strips these vars for every actual `make run`/`debug`
    # invocation, so this is a heads-up, not a required manual fix.
    local snap_polluted_vars=""
    for v in GTK_PATH GTK_EXE_PREFIX GTK_MODULES GIO_MODULE_DIR XDG_DATA_DIRS LOCPATH; do
        case "${!v:-}" in
            */snap/*) snap_polluted_vars="$snap_polluted_vars $v=${!v}" ;;
        esac
    done
    if [ -n "$snap_polluted_vars" ]; then
        warn "This shell has GTK/GIO/XDG env vars pointing into a snap bundle:"
        for kv in $snap_polluted_vars; do warn "  $kv"; done
        warn "This is almost always because the shell was spawned by a snap-packaged"
        warn "GUI app (e.g. VS Code's integrated terminal on Ubuntu). It can make any"
        warn "GTK app started from here — including QEMU with -display default — crash"
        warn "with 'undefined symbol __libc_pthread_init, GLIBC_PRIVATE'."
        warn "'make run'/'make debug' already strip these vars automatically before"
        warn "launching QEMU, so no action is needed there. If some other GTK tool in"
        warn "this repo ever misbehaves the same way, 'unset' the variables listed"
        warn "above first, or run it from a non-snap terminal."
    fi

    # grub-mkrescue is not an installable package on its own — it ships inside
    # grub-common (Ubuntu/Debian) or grub (Arch). Verify the binary actually
    # landed in PATH rather than assuming the package name guarantees it.
    if command -v grub-mkrescue >/dev/null 2>&1 || command -v i686-elf-grub-mkrescue >/dev/null 2>&1; then
        info "grub-mkrescue found (required for 'make release ARCH=amd64')."
    else
        warn "grub-mkrescue NOT found in PATH. The amd64 ISO release step will fail."
        warn "Re-check that grub-common (Ubuntu/Debian) or grub (Arch) installed correctly."
    fi

    echo
    info "make check ARCH=amd64:"; make -C "$REPO_ROOT" check ARCH=amd64 || true
    echo
    info "make check ARCH=aarch64:"; make -C "$REPO_ROOT" check ARCH=aarch64 || true
    echo
    info "Toolchain ready. Build: make all ARCH=amd64 | make all ARCH=aarch64"
}

# ==============================================================================
# 5. PATH sanity (defensive — /usr/local/bin is already default on virtually
#    every mainstream distro, but non-login/minimal shells or containers can
#    lack it; this is a no-op when it's already present)
# ==============================================================================
ensure_local_bin_in_path() {
    if ! echo "$PATH" | tr ':' '\n' | grep -qx "/usr/local/bin"; then
        warn "/usr/local/bin not found in current PATH."
        for rc in "$HOME/.bashrc" "$HOME/.zshrc"; do
            [ -f "$rc" ] || continue
            grep -qF 'export PATH="/usr/local/bin:$PATH"' "$rc" 2>/dev/null && continue
            echo 'export PATH="/usr/local/bin:$PATH"' >> "$rc"
            info "Added /usr/local/bin to PATH in $rc (reload shell or 'source $rc')."
        done
    fi
}

# ==============================================================================
# Main
# ==============================================================================
[ "$(uname -s)" = "Linux" ] || fail "This script targets Linux only."

install_packages
init_submodules
build_cross_toolchain
build_aarch64_none_elf_toolchain
verify_toolchain
ensure_local_bin_in_path