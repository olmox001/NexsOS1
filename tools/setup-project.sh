#!/usr/bin/env bash
# One-command, fresh-clone bootstrap for NexsOS1.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="$(uname -s)"

case "$HOST" in
  Darwin)
    SCRIPT="$ROOT/tools/setup-toolchain-macOS.sh"
    ;;
  Linux)
    SCRIPT="$ROOT/tools/setup-toolchain-linux.sh"
    ;;
  *)
    echo "[setup] ERROR: unsupported host '$HOST' (supported: macOS, Linux)." >&2
    exit 1
    ;;
esac

echo "[setup] Repository: $ROOT"
echo "[setup] Host: $HOST ($(uname -m))"
exec "$SCRIPT" "$@"
