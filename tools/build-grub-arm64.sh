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
lines = p.read_text().splitlines(keepends=True)
markers = (
    "/* Host-only compatibility for the GRUB 2.14 gnulib snapshot on Darwin. */",
    "/* Host-only declaration for the gnulib reallocarray replacement. */",
)
extern = "extern void *reallocarray (void *ptr, size_t nmemb, size_t size);"
for marker in markers:
    while True:
        start = next((i for i, line in enumerate(lines) if marker in line), None)
        if start is None:
            break
        end = next((i for i in range(start + 1, len(lines))
                    if lines[i].strip() == "#endif"
                    and extern in lines[i - 1]), None)
        if end is None:
            raise SystemExit("unterminated Darwin compatibility block in config-util.h")
        del lines[start:end + 1]

guard = next((i for i, line in enumerate(lines)
              if line.strip() == "#ifndef _GNU_SOURCE"), None)
if guard is None or guard + 1 >= len(lines) or    lines[guard + 1].strip() != "# define _GNU_SOURCE 1":
    raise SystemExit("cannot locate _GNU_SOURCE guard in GRUB config-util.h")
guard_end = next((i for i in range(guard + 2, len(lines))
                  if lines[i].strip() == "#endif"), None)
if guard_end is None:
    raise SystemExit("unterminated _GNU_SOURCE guard in GRUB config-util.h")
block = [
    "\n",
    "/* Host-only compatibility for the GRUB 2.14 gnulib snapshot on Darwin. */\n",
    "#if defined(__APPLE__)\n",
    "# include <stddef.h>\n",
    "# ifndef UINTPTR_WIDTH\n",
    "#  define UINTPTR_WIDTH (sizeof(void *) * 8U)\n",
    "# endif\n",
    "# ifndef FNM_EXTMATCH\n",
    "#  define FNM_EXTMATCH 0\n",
    "# endif\n",
    "extern void *reallocarray (void *ptr, size_t nmemb, size_t size);\n",
    "#endif\n",
]
lines[guard_end + 1:guard_end + 1] = block
p.write_text("".join(lines))
PY

    # The GRUB 2.14 tarball carries a stale generated gnulib wchar.h with
    # HAVE_FEATURES_H hard-coded to 1, even though configure detects 0 on
    # Darwin. Override it in the build directory only; do not patch vendor
    # source/cache files.
    python3 - "$SOURCE/grub-core/lib/gnulib/wchar.h" \
              "$BUILD/grub-core/lib/gnulib/wchar.h" \
              "$SOURCE/grub-core/lib/gnulib/string.h" \
              "$BUILD/grub-core/lib/gnulib/string.h" <<'PY'
from pathlib import Path
import sys
wchar_source, wchar_target, string_source, string_target = map(Path, sys.argv[1:])
wchar = wchar_source.read_text()
needle = "#if 1\n# include <features.h> /* for __GLIBC__ */\n#endif"
replacement = "#if 0 /* HAVE_FEATURES_H=0 on Darwin; avoid glibc-only header. */\n# include <features.h> /* for __GLIBC__ */\n#endif"
if needle not in wchar:
    raise SystemExit("cannot apply Darwin gnulib wchar.h compatibility override")
wchar = wchar.replace(needle, replacement, 1)
old = "#if 1\n# if !1\n_GL_FUNCDECL_SYS (wmempcpy,"
new = "#if 1\n# if 1 /* HAVE_WMEMPCPY=0; declare gnulib replacement. */\n_GL_FUNCDECL_SYS (wmempcpy,"
if old not in wchar:
    raise SystemExit("cannot apply Darwin gnulib wmempcpy declaration override")
wchar = wchar.replace(old, new, 1)

# Configure detects a working native mbrtowc on Darwin. The distributed
# generated header incorrectly enables rpl_mbrtowc and the tarball omits its
# replacement object, so select the system declaration/function.
old = ("#if 1\n# if 1\n#  if !(defined __cplusplus && defined GNULIB_NAMESPACE)\n"
       "#   undef mbrtowc\n#   define mbrtowc rpl_mbrtowc")
new = ("#if 1\n# if 0 /* HAVE_MBRTOWC=1; use native Darwin mbrtowc. */\n"
       "#  if !(defined __cplusplus && defined GNULIB_NAMESPACE)\n"
       "#   undef mbrtowc\n#   define mbrtowc rpl_mbrtowc")
if old not in wchar:
    raise SystemExit("cannot disable stale gnulib mbrtowc replacement on Darwin")
wchar = wchar.replace(old, new, 1)
wchar_target.parent.mkdir(parents=True, exist_ok=True)
wchar_target.write_text(wchar)

# The distributed generated string.h was produced for a host with mempcpy and
# memrchr. Configure detects both as absent on Darwin; expose their declarations
# so the gnulib replacement objects in libgnu.a are used.
string = string_source.read_text()
for symbol in ("mempcpy", "memrchr"):
    old = f"#if 1\n# if ! 1\n_GL_FUNCDECL_SYS ({symbol},"
    new = f"#if 1\n# if 1 /* HAVE_{symbol.upper()}=0; declare gnulib replacement. */\n_GL_FUNCDECL_SYS ({symbol},"
    if old not in string:
        raise SystemExit(f"cannot apply Darwin gnulib {symbol} declaration override")
    string = string.replace(old, new, 1)
string_target.parent.mkdir(parents=True, exist_ok=True)
string_target.write_text(string)
PY
fi

# GRUB's generated rules default to BSD awk on macOS. Force GNU awk for module
# dependency generation, which relies on GNU awk extensions. Apple ld also
# requires every 64-bit Mach-O member in a static archive to start on an
# 8-byte boundary. Darwin's /usr/bin/ar writes 2-byte-aligned members, which
# makes GRUB's host archives unusable by ld ("member ... not 8-byte aligned").
# Apple's libtool -static emits correctly aligned Mach-O archives; ranlib keeps
# that alignment when it adds/updates the symbol table. Keep GNU ar on other hosts.
HOST_ARCHIVE_ARGS=()
if [[ "$(uname -s)" == "Darwin" ]]; then
    HOST_ARCHIVE_ARGS=(AR=/usr/bin/libtool 'ARFLAGS=-static -o' RANLIB=/usr/bin/ranlib)
fi

echo "[grub-arm64] building host gnulib archive"
make -C "$BUILD/grub-core/lib/gnulib" -j"$JOBS" AWK=gawk "${HOST_ARCHIVE_ARGS[@]}" libgnu.a

echo "[grub-arm64] building host grub-mkimage"
make -C "$BUILD" -j"$JOBS" AWK=gawk "${HOST_ARCHIVE_ARGS[@]}" grub-mkimage

echo "[grub-arm64] building arm64-efi GRUB modules"
make -C "$BUILD/grub-core" -j"$JOBS" AWK=gawk "${HOST_ARCHIVE_ARGS[@]}" all

[[ -x "$BUILD/grub-mkimage" ]] || { echo "grub-mkimage was not produced" >&2; exit 1; }
[[ -f "$BUILD/grub-core/chain.mod" ]] || { echo "GRUB chain module missing" >&2; exit 1; }
[[ -f "$BUILD/grub-core/iso9660.mod" ]] || { echo "GRUB iso9660 module missing" >&2; exit 1; }
"$BUILD/grub-mkimage" --version | grep -F "$VERSION" >/dev/null
printf '%s' "$EXPECTED_INFO" > "$BUILD_INFO"
echo "[grub-arm64] ready: $BUILD/grub-mkimage"
