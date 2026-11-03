#!/usr/bin/env bash
#
# tools/setup-toolchain.sh
# ------------------------------------------------------------------------------
# Install the EXACT NexsOS1 / NEXS dual-arch cross toolchain on macOS (Homebrew),
# matching the project's known-good environment.  The Makefile and sources are
# never modified; everything lives in the toolchain.
#
#   amd64    : x86_64-elf-binutils + x86_64-elf-gcc      (homebrew/core)
#              -> prefix x86_64-elf-      (Makefile default)
#   aarch64  : aarch64-none-elf  ==  GCC 7.2.0           (tap sergiobenitez/osxct)
#              -> prefix aarch64-none-elf- (Makefile default)
#   emulator : qemu                                       (homebrew/core)
#   ISO only : i686-elf-grub + xorriso (optional, for `make release-iso`)
#
# WHY GCC 7.2.0 for aarch64 (do NOT "upgrade" it):
#   The kernel links -nostdlib (no libgcc) and boots a raw image.  GCC >= 10
#   (aarch64-elf 16, Arm 14.2, ...) default -moutline-atomics ON and use emulated
#   TLS, emitting __aarch64_*_sync / __emutls_get_address / abort that the
#   freestanding link cannot resolve; even when forced to link, the resulting
#   aarch64 kernel does NOT boot.  7.2.0 predates all of that and is the version
#   the repository is verified against (builds AND boots both arches).
#
# Idempotent: safe to re-run.
# ------------------------------------------------------------------------------
set -euo pipefail

info()  { printf '\033[1;34m[toolchain]\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m[toolchain]\033[0m %s\n' "$*"; }
fail()  { printf '\033[1;31m[toolchain]\033[0m %s\n' "$*" >&2; exit 1; }

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$REPO_ROOT/tools/toolchain-versions.env"

# 1. Preconditions ------------------------------------------------------------
[ "$(uname -s)" = "Darwin" ] || fail "This script targets macOS (Darwin)."
command -v brew >/dev/null 2>&1 || fail "Homebrew not found — install from https://brew.sh and re-run."
if [ "$(uname -m)" = "arm64" ]; then
  # osxct currently distributes its GCC 7.2.0 toolchain as an Intel binary.
  # Use the Intel Homebrew prefix explicitly; do not accidentally install the
  # formula into the native ARM Homebrew tree.
  [ -x /usr/local/bin/brew ] || fail "Apple Silicon requires Intel Homebrew at /usr/local/bin/brew for the pinned osxct toolchain. Install Rosetta 2 and Intel Homebrew, then re-run."
  BREW=/usr/local/bin/brew
  arch -x86_64 "$BREW" --version >/dev/null 2>&1 || fail "Rosetta 2 is unavailable. Run: softwareupdate --install-rosetta --agree-to-license"
else
  BREW="$(command -v brew)"
fi
brew() {
  if [ "$(uname -m)" = "arm64" ]; then
    arch -x86_64 "$BREW" "$@"
  else
    "$BREW" "$@"
  fi
}
if ! xcode-select -p >/dev/null 2>&1; then
  warn "Xcode Command Line Tools not detected — triggering install (a GUI dialog may appear)."
  xcode-select --install || true
  fail "Re-run this script once the CLT install completes."
fi
export HOMEBREW_NO_AUTO_UPDATE=1 NONINTERACTIVE=1

info "Initializing pinned project dependencies..."
"$REPO_ROOT/tools/setup-dependencies.sh" --init

brew_install_if_missing() {  # $1 = formula (may be fully qualified tap/name)
  local f="$1" name="${1##*/}"
  if brew list --formula "$name" >/dev/null 2>&1; then
    info "$name — already installed."
  else
    info "Installing $f ..."
    brew install "$f"
  fi
}

# 2. amd64 toolchain + QEMU (homebrew/core) -----------------------------------
brew_install_if_missing x86_64-elf-binutils
brew_install_if_missing x86_64-elf-gcc
brew_install_if_missing qemu

# 3. aarch64 toolchain — sergiobenitez/osxct aarch64-none-elf (GCC 7.2.0) ------
info "Tapping sergiobenitez/osxct (aarch64-none-elf 7.2.0) ..."
brew tap sergiobenitez/osxct >/dev/null 2>&1 || true
brew_install_if_missing sergiobenitez/osxct/aarch64-none-elf

# 4. Verify the locked compiler and linker versions --------------------------
verify_gcc_version() {
  local tool="$1" expected="$2" actual
  command -v "$tool" >/dev/null 2>&1 || fail "Required tool missing: $tool"
  actual="$("$tool" -dumpfullversion -dumpversion)"
  [ "$actual" = "$expected" ] || fail "$tool is $actual; expected locked version $expected."
  info "$tool $actual"
}
verify_binutils_version() {
  local tool="$1" expected="$2" actual
  command -v "$tool" >/dev/null 2>&1 || fail "Required tool missing: $tool"
  actual="$("$tool" --version | sed -E -n '1s/.* ([0-9]+([.][0-9]+)+)$/\1/p')"
  [ "$actual" = "$expected" ] || fail "$tool is $actual; expected locked version $expected."
  info "$tool $actual"
}
verify_gcc_version x86_64-elf-gcc "$NEXS_X86_64_GCC_VERSION"
verify_binutils_version x86_64-elf-ld "$NEXS_X86_64_BINUTILS_VERSION"
verify_gcc_version aarch64-none-elf-gcc "$NEXS_AARCH64_GCC_VERSION"
verify_binutils_version aarch64-none-elf-ld "$NEXS_AARCH64_BINUTILS_VERSION"

# 5. Optional: ISO/release tooling (only needed for `make release-iso`) --------
# Non-fatal: skip silently if unavailable.  `make all` / `make run` do not need these.
if [ "${NEXS_WITH_ISO:-0}" = "1" ]; then
  info "Installing optional ISO tooling (i686-elf-grub, xorriso) ..."
  brew_install_if_missing i686-elf-grub || warn "i686-elf-grub unavailable (ISO only)."
  brew_install_if_missing xorriso       || warn "xorriso unavailable (ISO only)."
else
  info "Skipping optional ISO tooling (set NEXS_WITH_ISO=1 to include i686-elf-grub/xorriso)."
fi

# 6. Report -------------------------------------------------------------------
echo
info "Installed versions:"
x86_64-elf-gcc --version       2>/dev/null | head -1 || warn "x86_64-elf-gcc missing"
aarch64-none-elf-gcc --version 2>/dev/null | head -1 || warn "aarch64-none-elf-gcc missing"
qemu-system-x86_64  --version  2>/dev/null | head -1 || warn "qemu-system-x86_64 missing"
qemu-system-aarch64 --version  2>/dev/null | head -1 || warn "qemu-system-aarch64 missing"

# Sanity: 7.2.0 must NOT emit outline-atomics (the marker of a too-new aarch64 GCC).
# Guard on the compiler existing first: with it missing, the probe pipeline would
# produce no output, grep would not match, and we would falsely report success.
if ! command -v aarch64-none-elf-gcc >/dev/null 2>&1; then
  warn "aarch64-none-elf-gcc not found — skipping the outline-atomics sanity check."
elif echo 'int x;int f(void){return __atomic_add_fetch(&x,1,5);}' \
     | aarch64-none-elf-gcc -O2 -S -o - -xc - 2>/dev/null | grep -q '__aarch64_'; then
  warn "aarch64 gcc emits outline-atomics — WRONG version (need 7.2.0 from osxct, not aarch64-elf 16/Arm 14)."
else
  info "aarch64 atomics inline (GCC 7.2.0 as expected)."
fi

echo
info "make check ARCH=amd64:";   make -C "$REPO_ROOT" check ARCH=amd64
echo
info "make check ARCH=aarch64:"; make -C "$REPO_ROOT" check ARCH=aarch64
echo
info "Toolchain ready. Build:  make all ARCH=amd64   |   make all ARCH=aarch64"
