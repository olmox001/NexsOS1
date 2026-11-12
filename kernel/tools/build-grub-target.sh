#!/usr/bin/env bash
set -euo pipefail

TARGET_ARCH="${1:-}"
case "$TARGET_ARCH" in
    aarch64)
        CROSS_TARGET="aarch64-none-elf"
        BUILD_FLAVOR="arm64-efi"
        PLATFORM="efi"
        MODULE_PLATFORM="arm64-efi"
        REQUIRED_MODULE="chain.mod"
        ;;
    x86_64)
        CROSS_TARGET="x86_64-elf"
        BUILD_FLAVOR="x86_64-efi"
        PLATFORM="efi"
        MODULE_PLATFORM="x86_64-efi"
        REQUIRED_MODULE="multiboot2.mod"
        ;;
    i386)
        CROSS_TARGET="i686-elf"
        BUILD_FLAVOR="i386-pc"
        PLATFORM="pc"
        MODULE_PLATFORM="i386-pc"
        REQUIRED_MODULE="multiboot2.mod"
        ;;
    *)
        echo "usage: $0 {aarch64|x86_64|i386}" >&2
        exit 2
        ;;
esac
LOG_PREFIX="grub-${BUILD_FLAVOR}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_ROOT="$(cd "$ROOT/.." && pwd)"
GRUB_MANIFEST="$PROJECT_ROOT/boot/grub/manifest.env"
[[ -f "$GRUB_MANIFEST" ]] || {
    echo "[$LOG_PREFIX] missing GRUB manifest: $GRUB_MANIFEST" >&2
    exit 1
}
# shellcheck disable=SC1091
set -a
source "$ROOT/config/toolchain-versions.env"
source "$GRUB_MANIFEST"
set +a

VERSION="$NEXS_GRUB_VERSION"
PIPELINE_REVISION="$NEXS_GRUB_PIPELINE_REVISION"
GRUB_COMMIT="$NEXS_GRUB_COMMIT"
GRUB_PATCH_DIR="$ROOT/tools/patches/grub-2.14"
GNULIB_REVISION="$NEXS_GRUB_GNULIB_REVISION"
UPSTREAM="${NEXS_GRUB_SOURCE:-$PROJECT_ROOT/boot/grub/upstream}"
BUILD_ROOT="${NEXS_BUILD_ROOT:-$PROJECT_ROOT/build}"
case "$BUILD_ROOT" in
    /*) ;;
    *) echo "[$LOG_PREFIX] NEXS_BUILD_ROOT must be absolute: $BUILD_ROOT" >&2; exit 1 ;;
esac
CACHE="$BUILD_ROOT/cache/grub"
PREBUILT="$CACHE/prebuilt/$BUILD_FLAVOR"
SOURCE="$CACHE/git-${GRUB_COMMIT:0:8}"
SOURCE_INFO="$SOURCE/.nexs-bootstrap-info"
GNULIB_CACHE="$CACHE/gnulib-$GNULIB_REVISION"
BUILD="$BUILD_ROOT/grub/$BUILD_FLAVOR"
JOBS="${NEXS_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
HOST_KEY="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
PATCH_FILES=("$GRUB_PATCH_DIR"/*.patch)
[[ -f "${PATCH_FILES[0]}" ]] || {
    echo "[$LOG_PREFIX] no GRUB compatibility patches in: $GRUB_PATCH_DIR" >&2
    exit 1
}
for patch_file in "${PATCH_FILES[@]}"; do
    [[ -f "$patch_file" ]] || {
        echo "[$LOG_PREFIX] invalid patch entry: $patch_file" >&2
        exit 1
    }
done
GRUB_PATCHSET_SHA256="$(python3 - "$GRUB_PATCH_DIR" <<'PY'
from hashlib import sha256
from pathlib import Path
import sys

root = Path(sys.argv[1])
patches = sorted(root.glob("*.patch"), key=lambda item: item.name)
if not patches:
    raise SystemExit("empty GRUB patchset")
digest = sha256()
for patch in patches:
    digest.update(patch.name.encode("utf-8"))
    digest.update(b"\0")
    digest.update(patch.read_bytes())
    digest.update(b"\0")
print(digest.hexdigest())
PY
)"
EXPECTED_INFO="$(printf 'PIPELINE=%s\nGRUB=%s\nGRUB_COMMIT=%s\nGNULIB=%s\nPATCHSET=%s\nHOST=%s\nTARGET=%s\nPLATFORM=%s\nMODULE_PLATFORM=%s' \
    "$PIPELINE_REVISION" "$VERSION" "$GRUB_COMMIT" "$GNULIB_REVISION" \
    "$GRUB_PATCHSET_SHA256" "$HOST_KEY" "$CROSS_TARGET" "$PLATFORM" "$MODULE_PLATFORM")"

prebuilt_ready() {
    [[ -x "$PREBUILT/bin/grub-mkimage" ]] || return 1
    [[ -f "$PREBUILT/lib/grub/$MODULE_PLATFORM/$REQUIRED_MODULE" ]] || return 1
    [[ -f "$PREBUILT/nexs-grub-build-info.txt" ]] || return 1
    [[ "$(cat "$PREBUILT/nexs-grub-build-info.txt")" == "$EXPECTED_INFO" ]] || return 1
    if [[ "$PLATFORM" == "pc" ]]; then
        [[ -x "$PREBUILT/bin/grub-mkrescue" ]] || return 1
    fi
}

if [[ "${NEXS_GRUB_FORCE_REBUILD:-0}" != "1" ]] && prebuilt_ready; then
    echo "[$LOG_PREFIX] reusing verified prebuilt: $PREBUILT"
    exit 0
fi

for tool in git tar make gawk python3 patch autoconf automake autopoint \
            "$CROSS_TARGET-gcc" "$CROSS_TARGET-objcopy"; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "[$LOG_PREFIX] missing required tool: $tool" >&2
        exit 1
    }
done

[[ -d "$UPSTREAM/.git" || -f "$UPSTREAM/.git" ]] || {
    echo "[$LOG_PREFIX] missing GRUB submodule: $UPSTREAM" >&2
    exit 1
}
ACTUAL_COMMIT="$(git -C "$UPSTREAM" rev-parse HEAD)"
[[ "$ACTUAL_COMMIT" == "$GRUB_COMMIT" ]] || {
    echo "[$LOG_PREFIX] GRUB submodule commit mismatch: $ACTUAL_COMMIT" >&2
    exit 1
}
PATCH_FILES=("$GRUB_PATCH_DIR"/*.patch)
[[ -f "${PATCH_FILES[0]}" ]] || {
    echo "[$LOG_PREFIX] no GRUB compatibility patches in: $GRUB_PATCH_DIR" >&2
    exit 1
}
for patch_file in "${PATCH_FILES[@]}"; do
    [[ -f "$patch_file" ]] || {
        echo "[$LOG_PREFIX] invalid patch entry: $patch_file" >&2
        exit 1
    }
done
GRUB_PATCHSET_SHA256="$(python3 - "$GRUB_PATCH_DIR" <<'PY'
from hashlib import sha256
from pathlib import Path
import sys

root = Path(sys.argv[1])
patches = sorted(root.glob("*.patch"), key=lambda item: item.name)
if not patches:
    raise SystemExit("empty GRUB patchset")
digest = sha256()
for patch in patches:
    digest.update(patch.name.encode("utf-8"))
    digest.update(b"\0")
    digest.update(patch.read_bytes())
    digest.update(b"\0")
print(digest.hexdigest())
PY
)"

mkdir -p "$CACHE"
if [[ ! -d "$GNULIB_CACHE/.git" ]] || \
   [[ "$(git -C "$GNULIB_CACHE" rev-parse HEAD 2>/dev/null || true)" != "$GNULIB_REVISION" ]]; then
    rm -rf "$GNULIB_CACHE"
    git init "$GNULIB_CACHE"
    git -C "$GNULIB_CACHE" remote add origin https://git.savannah.gnu.org/git/gnulib.git
    git -C "$GNULIB_CACHE" fetch --depth 1 origin "$GNULIB_REVISION"
    git -C "$GNULIB_CACHE" checkout --detach FETCH_HEAD
fi

SOURCE_REBUILT=0
EXPECTED_SOURCE_INFO="$(printf 'GRUB_COMMIT=%s\\nGNULIB=%s\\nPATCHSET_SHA256=%s' \
    "$GRUB_COMMIT" "$GNULIB_REVISION" "$GRUB_PATCHSET_SHA256")"
if [[ ! -x "$SOURCE/configure" ]] || [[ ! -f "$SOURCE_INFO" ]] || \
   [[ "$(cat "$SOURCE_INFO" 2>/dev/null || true)" != "$EXPECTED_SOURCE_INFO" ]]; then
    echo "[$LOG_PREFIX] rebuilding GRUB source cache from pinned submodule"
    rm -rf "$SOURCE"
    mkdir -p "$SOURCE"
    git -C "$UPSTREAM" archive "$GRUB_COMMIT" | tar -x -C "$SOURCE"
    for patch_file in "${PATCH_FILES[@]}"; do
        echo "[$LOG_PREFIX] applying $(basename "$patch_file")"
        patch --batch --forward -d "$SOURCE" -p1 < "$patch_file"
    done
    (
        cd "$SOURCE"
        ./bootstrap --skip-po --no-git --gnulib-srcdir="$GNULIB_CACHE"
    )
    printf '%s' "$EXPECTED_SOURCE_INFO" > "$SOURCE_INFO"
    SOURCE_REBUILT=1
fi

TRIPLET="$("$SOURCE/build-aux/config.guess")"
BUILD_INFO="$BUILD/nexs-build-info.txt"
if [[ "$SOURCE_REBUILT" == 1 ]] || \
   { [[ -f "$BUILD/Makefile" ]] && [[ ! -f "$BUILD_INFO" ]]; } || \
   { [[ -f "$BUILD_INFO" ]] && [[ "$(cat "$BUILD_INFO")" != "$EXPECTED_INFO" ]]; }; then
    echo "[$LOG_PREFIX] source/host/configuration changed; recreating generated build directory"
    case "$BUILD" in "$BUILD_ROOT"/grub/"$BUILD_FLAVOR") rm -rf "$BUILD" ;; *) echo "unsafe build path: $BUILD" >&2; exit 1 ;; esac
fi
mkdir -p "$BUILD"

if [[ ! -f "$BUILD/Makefile" ]]; then
    echo "[$LOG_PREFIX] configuring GRUB $VERSION for $BUILD_FLAVOR (host $TRIPLET)"
    (cd "$BUILD" && AWK=gawk "$SOURCE/configure" \
        --build="$TRIPLET" \
        --host="$TRIPLET" \
        --target="$CROSS_TARGET" \
        --with-platform="$PLATFORM" \
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
        # Remove the separator blank line too; otherwise each invocation
        # inserts another one and touches config-util.h, forcing a full rebuild.
        while start > 0 and not lines[start - 1].strip():
            start -= 1
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
content = "".join(lines)
if p.read_text() != content:
    p.write_text(content)
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

# Build the gnulib subdirectory through its normal Automake target. Directly
# asking for libgnu.a skips part of BUILT_SOURCES (stdio.h/getopt.h/locale.h
# and other configured wrappers), making compilation accidentally consume
# host SDK headers. Serial generation also keeps the generated-header graph
# deterministic before the parallel top-level GRUB build.
echo "[$LOG_PREFIX] generating configured gnulib headers and archive"
make -C "$BUILD/grub-core/lib/gnulib" -j1 AWK=gawk "${HOST_ARCHIVE_ARGS[@]}" all

python3 - "$BUILD/grub-core/lib/gnulib/wchar.h" \
          "$BUILD/grub-core/lib/gnulib/string.h" \
          "$BUILD/grub-core/lib/gnulib/libgnu.a" <<'PY'
from pathlib import Path
import sys

wchar_path, string_path, archive_path = map(Path, sys.argv[1:])
for path in (wchar_path, string_path, archive_path):
    if not path.is_file() or path.stat().st_size == 0:
        raise SystemExit(f"missing generated GRUB host artifact: {path}")

wchar = wchar_path.read_text()
string = string_path.read_text()
checks = (
    ("wchar HAVE_FEATURES_H", "#if 0\n# include <features.h> /* for __GLIBC__ */\n#endif", wchar),
    ("wchar native mbrtowc", "#if 1\n# if 0\n#  if !(defined __cplusplus && defined GNULIB_NAMESPACE)\n#   undef mbrtowc\n#   define mbrtowc rpl_mbrtowc", wchar),
    ("wchar replacement declaration", "#if 1\n# if !0\n_GL_FUNCDECL_SYS (wmempcpy,", wchar),
    ("string mempcpy declaration", "#if 1\n# if ! 0\n_GL_FUNCDECL_SYS (mempcpy,", string),
    ("string memrchr declaration", "#if 1\n# if ! 0\n_GL_FUNCDECL_SYS (memrchr,", string),
)
for label, needle, content in checks:
    if needle not in content:
        raise SystemExit(f"unexpected GRUB gnulib configure result: {label}")
PY

# GRUB 2.14 generates grub_script.tab.h as a side effect of the bison
# grub_script.tab.c target. Its generated module rules do not consistently
# express that prerequisite before compiling yylex.l in parallel. Materialize
# the parser/header serially before launching the parallel EFI module build.
echo "[$LOG_PREFIX] generating script parser/header before parallel module compilation"
make -C "$BUILD/grub-core" -j1 AWK=gawk grub_script.tab.c
if [[ ! -s "$BUILD/grub-core/grub_script.tab.h" ]]; then
    echo "[$LOG_PREFIX] bison did not produce grub_script.tab.h" >&2
    exit 1
fi

echo "[$LOG_PREFIX] building host GRUB image tools"
HOST_TOOL_TARGETS=(grub-mkimage)
if [[ "$PLATFORM" == "pc" ]]; then
    HOST_TOOL_TARGETS+=(grub-mkrescue)
fi
make -C "$BUILD" -j"$JOBS" AWK=gawk "${HOST_ARCHIVE_ARGS[@]}" "${HOST_TOOL_TARGETS[@]}"

echo "[$LOG_PREFIX] building $BUILD_FLAVOR GRUB modules"
make -C "$BUILD/grub-core" -j"$JOBS" AWK=gawk "${HOST_ARCHIVE_ARGS[@]}" all

test -x "$BUILD/grub-mkimage" || {
    echo "[$LOG_PREFIX] grub-mkimage was not produced: $BUILD/grub-mkimage" >&2
    exit 1
}
if [[ "$PLATFORM" == "pc" ]]; then
    test -x "$BUILD/grub-mkrescue" || {
        echo "[$LOG_PREFIX] grub-mkrescue was not produced: $BUILD/grub-mkrescue" >&2
        exit 1
    }
fi
test -f "$BUILD/grub-core/$REQUIRED_MODULE" || {
    echo "[$LOG_PREFIX] required module missing: $REQUIRED_MODULE" >&2
    exit 1
}
test -f "$BUILD/grub-core/iso9660.mod" || {
    echo "[$LOG_PREFIX] GRUB iso9660 module missing" >&2
    exit 1
}
"$BUILD/grub-mkimage" --version | grep -F "$VERSION" >/dev/null

echo "[$LOG_PREFIX] installing reusable target stage"
make -C "$BUILD" -j"$JOBS" AWK=gawk "${HOST_ARCHIVE_ARGS[@]}" install
STAGE="$BUILD/stage"
MODULE_DIR="$STAGE/lib/grub/$MODULE_PLATFORM"
test -x "$STAGE/bin/grub-mkimage" || {
    echo "[$LOG_PREFIX] installed grub-mkimage missing" >&2
    exit 1
}
test -f "$MODULE_DIR/$REQUIRED_MODULE" || {
    echo "[$LOG_PREFIX] installed module missing: $MODULE_DIR/$REQUIRED_MODULE" >&2
    exit 1
}
if [[ "$PLATFORM" == "pc" ]]; then
    test -x "$STAGE/bin/grub-mkrescue" || {
        echo "[$LOG_PREFIX] installed grub-mkrescue missing" >&2
        exit 1
    }
fi

printf '%s' "$EXPECTED_INFO" > "$BUILD_INFO"
printf '%s\n' "$EXPECTED_INFO" > "$STAGE/nexs-grub-build-info.txt"

echo "[$LOG_PREFIX] publishing reusable cache stage"
PREBUILT_PARENT="$CACHE/prebuilt"
PREBUILT_TMP="$PREBUILT_PARENT/.$BUILD_FLAVOR.tmp.$$"
mkdir -p "$PREBUILT_PARENT"
rm -rf "$PREBUILT_TMP"
cp -R "$STAGE" "$PREBUILT_TMP"
rm -rf "$PREBUILT"
mv "$PREBUILT_TMP" "$PREBUILT"
prebuilt_ready || {
    echo "[$LOG_PREFIX] published prebuilt failed verification" >&2
    exit 1
}
echo "[$LOG_PREFIX] ready: $PREBUILT"
