#!/usr/bin/env bash
set -euo pipefail

ARCHIVE="${1:-}"
[[ -n "$ARCHIVE" ]] || {
    echo "usage: $0 <grub-prebuilt.tar.gz>" >&2
    exit 2
}
[[ -f "$ARCHIVE" ]] || {
    echo "[grub-install] archive not found: $ARCHIVE" >&2
    exit 1
}

KERNEL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_ROOT="$(cd "$KERNEL_ROOT/.." && pwd)"
BUILD_ROOT="${NEXS_BUILD_ROOT:-$PROJECT_ROOT/build}"
LOCK="$PROJECT_ROOT/boot/grub/manifest.env"

[[ "$BUILD_ROOT" = /* ]] || {
    echo "[grub-install] NEXS_BUILD_ROOT must be absolute" >&2
    exit 1
}
# shellcheck disable=SC1090
source "$LOCK"

host_tag() {
    printf '%s-%s' "$(uname -s | tr '[:upper:]' '[:lower:]')" "$(uname -m)"
}

manifest_value() {
    local key="$1"
    local file="$2"
    sed -n "s/^${key}=//p" "$file" | head -1
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/nexs-grub.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

python3 - "$ARCHIVE" <<'PY'
from pathlib import PurePosixPath
import sys
import tarfile

archive = sys.argv[1]
with tarfile.open(archive, "r:gz") as stream:
    for member in stream.getmembers():
        path = PurePosixPath(member.name)
        if path.is_absolute() or ".." in path.parts:
            raise SystemExit(f"unsafe archive member: {member.name}")
        if member.issym() or member.islnk():
            target = PurePosixPath(member.linkname)
            if target.is_absolute() or ".." in target.parts:
                raise SystemExit(
                    f"unsafe archive link: {member.name} -> {member.linkname}"
                )
PY
tar -xzf "$ARCHIVE" -C "$TMP"
TARGET=""
for candidate in i386-pc x86_64-efi arm64-efi; do
    if [[ -f "$TMP/prebuilt/$candidate/prebuilt-manifest.txt" ]]; then
        if [[ -n "$TARGET" ]]; then
            echo "[grub-install] archive contains multiple targets" >&2
            exit 1
        fi
        TARGET="$candidate"
    fi
done
[[ -n "$TARGET" ]] || {
    echo "[grub-install] no supported prebuilt target in archive" >&2
    exit 1
}

SOURCE="$TMP/prebuilt/$TARGET"
MANIFEST="$SOURCE/prebuilt-manifest.txt"
[[ -f "$SOURCE/nexs-grub-build-info.txt" ]] || {
    echo "[grub-install] missing build provenance" >&2
    exit 1
}

VERSION="$(manifest_value grub_version "$MANIFEST")"
COMMIT="$(manifest_value grub_commit "$MANIFEST")"
REVISION="$(manifest_value pipeline_revision "$MANIFEST")"
HOST="$(manifest_value host "$MANIFEST")"
EXPECTED_TREE="$(manifest_value stage_tree_sha256 "$MANIFEST")"

[[ "$VERSION" == "$NEXS_GRUB_VERSION" ]] || {
    echo "[grub-install] GRUB version mismatch: $VERSION" >&2
    exit 1
}
[[ "$COMMIT" == "$NEXS_GRUB_COMMIT" ]] || {
    echo "[grub-install] GRUB commit mismatch: $COMMIT" >&2
    exit 1
}
[[ "$REVISION" == "$NEXS_GRUB_PIPELINE_REVISION" ]] || {
    echo "[grub-install] pipeline revision mismatch: $REVISION" >&2
    exit 1
}
[[ "$HOST" == "$(host_tag)" ]] || {
    echo "[grub-install] host mismatch: package=$HOST local=$(host_tag)" >&2
    exit 1
}
ACTUAL_TREE="$(tree_hash "$SOURCE")"
[[ "$ACTUAL_TREE" == "$EXPECTED_TREE" ]] || {
    echo "[grub-install] stage hash mismatch" >&2
    exit 1
}

DEST="$BUILD_ROOT/cache/grub/prebuilt/$TARGET"
case "$DEST" in
    "$BUILD_ROOT"/cache/grub/prebuilt/i386-pc|    "$BUILD_ROOT"/cache/grub/prebuilt/x86_64-efi|    "$BUILD_ROOT"/cache/grub/prebuilt/arm64-efi) ;;
    *) echo "[grub-install] unsafe destination: $DEST" >&2; exit 1 ;;
esac

mkdir -p "$(dirname "$DEST")"
TMP_DEST="$(dirname "$DEST")/.$TARGET.install.$$"
rm -rf "$TMP_DEST"
cp -R "$SOURCE" "$TMP_DEST"
rm -rf "$DEST"
mv "$TMP_DEST" "$DEST"
echo "[grub-install] installed $TARGET -> $DEST"
