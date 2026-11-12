#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -x "$ROOT/system/tools/setup-toolchain-macos.sh" ] || {
  echo "[toolchain] NX-SDK tooling is unavailable; run tools/setup-dependencies.sh --init" >&2
  exit 1
}
exec "$ROOT/system/tools/setup-toolchain-macos.sh" "$@"
