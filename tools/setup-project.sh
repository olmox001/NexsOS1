#!/usr/bin/env bash
# One-command, fresh-clone bootstrap for the NexsOS1 integration checkout.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="$(uname -s)"
echo "[setup] Repository: $ROOT"
echo "[setup] Host: $HOST ($(uname -m))"
"$ROOT/tools/setup-dependencies.sh" --init
case "$HOST" in
  Darwin) exec "$ROOT/system/tools/setup-toolchain-macos.sh" "$@" ;;
  Linux) exec "$ROOT/system/tools/setup-toolchain-linux.sh" "$@" ;;
  *) echo "[setup] ERROR: unsupported host '$HOST' (supported: macOS, Linux)." >&2; exit 1 ;;
esac
