#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
set -a
source "$ROOT/tools/toolchain-versions.env"
set +a

VERSION="${NEXS_GRUB_VERSION:-2.14}"
SHA512="${NEXS_GRUB_SHA512:-5008e3b70072798d2b34301219fe174cc6bcfead40a9ab25fe32f30c62a392ffb7bf2eb629d481f4a6c19c2925324af3f08d7043e62623e746be2d8b3cbd1182}"
CACHE="$ROOT/build/cache/grub"
TARBALL="$CACHE/grub-$VERSION.tar.xz"
SOURCE_ROOT="$CACHE/src"
SOURCE="$SOURCE_ROOT/grub-$VERSION"
BUILD="$ROOT/build/grub/arm64-efi"
JOBS="${NEXS_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

for tool in curl tar make gawk python3 aarch64-none-elf-gcc aarch64-none-elf-objcopy; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "[grub-arm64] missing required tool: $tool" >&2
        exit 1
    }
done

mkdir -p "$CACHE" "$SOURCE_ROOT"
if [[ ! -f "$TARBALL" ]]; then
    echo "[grub-arm64] downloading GNU GRUB $VERSION"
    curl -fL --retry 3 "https://ftp.gnu.org/gnu/grub/grub-$VERSION.tar.xz" -o "$TARBALL"
fi

if command -v sha512sum >/dev/null 2>&1; then
    printf '%s  %s\n' "$SHA512" "$TARBALL" | sha512sum -c -
else
    printf '%s  %s\n' "$SHA512" "$TARBALL" | shasum -a 512 -c -
fi

if [[ ! -f "$SOURCE/configure" ]]; then
    tar -xJf "$TARBALL" -C "$SOURCE_ROOT"
fi

TRIPLET="$("$SOURCE/build-aux/config.guess")"
BUILD_INFO="$BUILD/nexs-build-info.txt"
EXPECTED_INFO="$(printf 'GRUB=%s\nHOST=%s\nTARGET=aarch64-none-elf\nPLATFORM=efi\nSHA512=%s' "$VERSION" "$TRIPLET" "$SHA512")"
if [[ -f "$BUILD_INFO" ]] && [[ "$(cat "$BUILD_INFO")" != "$EXPECTED_INFO" ]]; then
    echo "[grub-arm64] host/configuration changed; recreating generated build directory"
    case "$BUILD" in "$ROOT"/build/grub/arm64-efi) rm -rf "$BUILD" ;; *) echo "unsafe build path: $BUILD" >&2; exit 1 ;; esac
fi
mkdir -p "$BUILD"

if [[ ! -f "$BUILD/Makefile" ]]; then
    echo "[grub-arm64] configuring GRUB $VERSION for arm64-efi (host $TRIPLET)"
    (cd "$BUILD" && AWK=gawk "$SOURCE/configure" \
        --build="$TRIPLET" \
        --host="$TRIPLET" \
        --target=aarch64-none-elf \
        --with-platform=efi \
        --prefix="$BUILD/stage" \
        --disable-nls \
        --disable-grub-mkfont \
        --disable-grub-mount)
fi

# GRUB 2.14's generated host header lacks Apple's reallocarray prototype.
# Keep this narrowly scoped to the generated build tree; never patch vendor source.
if [[ "$(uname -s)" == "Darwin" ]]; then
    python3 - "$BUILD/config-util.h" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
needle = "#ifndef _GNU_SOURCE\n# define _GNU_SOURCE 1\n"
insert = (needle + "\n/* Host-only declaration for the gnulib reallocarray replacement. */\n"
          "#if defined(__APPLE__)\n# include <stddef.h>\n"
          "extern void *reallocarray (void *ptr, size_t nmemb, size_t size);\n"
          "#endif\n")
if "extern void *reallocarray (void *ptr, size_t nmemb, size_t size);" not in s:
    if needle not in s:
        raise SystemExit("cannot apply macOS reallocarray compatibility patch")
    p.write_text(s.replace(needle, insert, 1))
PY
fi

# GRUB's generated rules default to BSD awk on macOS. Force GNU awk for module
# dependency generation, which relies on GNU awk extensions.
echo "[grub-arm64] building host grub-mkimage"
make -C "$BUILD" -j"$JOBS" AWK=gawk grub-mkimage

echo "[grub-arm64] building arm64-efi GRUB modules"
make -C "$BUILD/grub-core" -j"$JOBS" AWK=gawk all

[[ -x "$BUILD/grub-mkimage" ]] || { echo "grub-mkimage was not produced" >&2; exit 1; }
[[ -f "$BUILD/grub-core/chain.mod" ]] || { echo "GRUB chain module missing" >&2; exit 1; }
[[ -f "$BUILD/grub-core/iso9660.mod" ]] || { echo "GRUB iso9660 module missing" >&2; exit 1; }
"$BUILD/grub-mkimage" --version | grep -F "$VERSION" >/dev/null
printf '%s' "$EXPECTED_INFO" > "$BUILD_INFO"
echo "[grub-arm64] ready: $BUILD/grub-mkimage"
