#!/usr/bin/env bash
set -euo pipefail

TARGET="${1:-all}"
KERNEL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_ROOT="$(cd "$KERNEL_ROOT/.." && pwd)"
BUILD_ROOT="${NEXS_BUILD_ROOT:-$PROJECT_ROOT/build}"
RELEASE_ROOT="${NEXS_GRUB_RELEASE_ROOT:-$PROJECT_ROOT/release/grub}"
LOCK="$PROJECT_ROOT/boot/grub/manifest.env"

[[ "$BUILD_ROOT" = /* ]] || {
    echo "[grub-package] NEXS_BUILD_ROOT must be absolute" >&2
    exit 1
}
[[ -f "$LOCK" ]] || {
    echo "[grub-package] missing $LOCK" >&2
    exit 1
}
# shellcheck disable=SC1090
source "$LOCK"

host_tag() {
    printf '%s-%s' "$(uname -s | tr '[:upper:]' '[:lower:]')" "$(uname -m)"
}

target_arg() {
    case "$1" in
        i386-pc) printf '%s' i386 ;;
        x86_64-efi) printf '%s' x86_64 ;;
        arm64-efi) printf '%s' aarch64 ;;
        *) return 1 ;;
    esac
}

tree_hash() {
    python3 - "$1" <<'PY'
from hashlib import sha256
from pathlib import Path
import os
import stat
import sys

root = Path(sys.argv[1])
digest = sha256()
for path in sorted(root.rglob("*"), key=lambda item: item.as_posix()):
    if path.name == "prebuilt-manifest.txt":
        continue
    rel = path.relative_to(root).as_posix().encode()
    st = path.lstat()
    digest.update(rel + b"\0")
    digest.update(oct(stat.S_IMODE(st.st_mode)).encode() + b"\0")
    if path.is_symlink():
        digest.update(b"L\0" + os.readlink(path).encode() + b"\0")
    elif path.is_file():
        digest.update(
            b"F\0" + sha256(path.read_bytes()).hexdigest().encode() + b"\0"
        )
    elif path.is_dir():
        digest.update(b"D\0")
print(digest.hexdigest())
PY
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

package_one() {
    local flavor="$1"
    local arg
    local prebuilt
    local host
    local out_dir
    local package
    local stage_hash

    arg="$(target_arg "$flavor")" || {
        echo "[grub-package] unsupported target: $flavor" >&2
        exit 2
    }
    env NEXS_BUILD_ROOT="$BUILD_ROOT"         "$KERNEL_ROOT/tools/build-grub-target.sh" "$arg"

    prebuilt="$BUILD_ROOT/cache/grub/prebuilt/$flavor"
    [[ -d "$prebuilt" && -f "$prebuilt/nexs-grub-build-info.txt" ]] || {
        echo "[grub-package] incomplete prebuilt stage: $prebuilt" >&2
        exit 1
    }

    host="$(host_tag)"
    out_dir="$RELEASE_ROOT/$NEXS_GRUB_VERSION/$host"
    mkdir -p "$out_dir"
    package="$out_dir/grub-$NEXS_GRUB_VERSION-${NEXS_GRUB_COMMIT:0:8}-$flavor-$host.tar.gz"
    stage_hash="$(tree_hash "$prebuilt")"

    {
        echo "format=1"
        echo "grub_version=$NEXS_GRUB_VERSION"
        echo "grub_commit=$NEXS_GRUB_COMMIT"
        echo "pipeline_revision=$NEXS_GRUB_PIPELINE_REVISION"
        echo "target=$flavor"
        echo "host=$host"
        echo "source=boot/grub/upstream"
        echo "stage_tree_sha256=$stage_hash"
    } > "$prebuilt/prebuilt-manifest.txt"

    COPYFILE_DISABLE=1 tar -C "$BUILD_ROOT/cache/grub"         -cf - "prebuilt/$flavor" | gzip -n > "$package"
    printf '%s  %s\n' "$(sha256_file "$package")" "$(basename "$package")" \
        > "$package.sha256"
    printf '[grub-package] %s\n' "$package"
}

case "$TARGET" in
    all)
        for target in $NEXS_GRUB_TARGETS; do
            package_one "$target"
        done
        ;;
    i386-pc|x86_64-efi|arm64-efi)
        package_one "$TARGET"
        ;;
    *)
        echo "usage: $0 {all|i386-pc|x86_64-efi|arm64-efi}" >&2
        exit 2
        ;;
esac
