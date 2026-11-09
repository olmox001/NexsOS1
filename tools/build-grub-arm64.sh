#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${NEXS_BUILD_ROOT:-$ROOT/build}"
exec env NEXS_BUILD_ROOT="$BUILD_ROOT" "$ROOT/kernel/tools/build-grub-arm64.sh" "$@"
