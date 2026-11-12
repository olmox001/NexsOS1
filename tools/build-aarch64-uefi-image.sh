#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${NEXS_BUILD_ROOT:-$ROOT/build}"
DTB="${NEXS_DTB_PATH:-$BUILD_ROOT/aarch64/virt.dtb}"
exec env NEXS_BUILD_ROOT="$BUILD_ROOT" NEXS_DTB_PATH="$DTB" \
    "$ROOT/kernel/tools/build-aarch64-uefi-image.sh" "$@"
