#!/usr/bin/env bash
# Archive the exact tracked legacy kernel at HEAD and verify a full restore.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REV="$(git -C "$ROOT" rev-parse HEAD)"
SHORT_REV="${REV:0:16}"
ARCHIVE_DIR="$ROOT/backups"
ARCHIVE="$ARCHIVE_DIR/legacy-kernel-$SHORT_REV.tar.gz"
VERIFY_DIR="$ROOT/build/legacy-kernel-restore-check"

git -C "$ROOT" diff --quiet "$REV" -- kernel || {
  echo "[kernel-backup] ERROR: unstaged kernel changes exist; refusing an ambiguous backup." >&2
  exit 1
}
git -C "$ROOT" diff --cached --quiet "$REV" -- kernel || {
  echo "[kernel-backup] ERROR: staged kernel changes exist; refusing an ambiguous backup." >&2
  exit 1
}
if [ -n "$(git -C "$ROOT" ls-files --others --exclude-standard kernel)" ]; then
  echo "[kernel-backup] ERROR: untracked kernel files exist; review before archiving." >&2
  exit 1
fi
if [ "$(git -C "$ROOT" ls-tree -r "$REV" kernel | awk '$1 == "160000" {n++} END {print n+0}')" -ne 0 ]; then
  echo "[kernel-backup] ERROR: kernel contains gitlinks; archive them separately first." >&2
  exit 1
fi

mkdir -p "$ARCHIVE_DIR" "$ROOT/build"
git -C "$ROOT" archive --format=tar "$REV" kernel | gzip -9 > "$ARCHIVE"
if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "$ARCHIVE" > "$ARCHIVE.sha256"
else
  shasum -a 256 "$ARCHIVE" > "$ARCHIVE.sha256"
fi

rm -rf "$VERIFY_DIR"
mkdir -p "$VERIFY_DIR"
tar -xzf "$ARCHIVE" -C "$VERIFY_DIR"
diff -qr "$ROOT/kernel" "$VERIFY_DIR/kernel" > "$VERIFY_DIR/diff.txt"
tar -tzf "$ARCHIVE" >/dev/null

{
  echo "NEXS legacy kernel backup"
  echo "source_revision=$REV"
  echo "source_path=kernel/"
  echo "tracked_files=$(git -C "$ROOT" ls-tree -r "$REV" kernel | wc -l | tr -d ' ')"
  echo "source_size=$(du -sh "$ROOT/kernel" | awk '{print $1}')"
  echo "archive=$ARCHIVE"
  echo "archive_sha256=$(awk '{print $1}' "$ARCHIVE.sha256")"
  echo "restore_verification=PASS (tar listing + extracted tree diff)"
  echo "created_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
} > "$ARCHIVE.manifest.txt"

echo "[kernel-backup] PASS"
cat "$ARCHIVE.manifest.txt"
