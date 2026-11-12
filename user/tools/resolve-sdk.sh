#!/usr/bin/env bash
set -euo pipefail
USERLAND_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:---print}"
SDK_REPOSITORY="${NEXS_SDK_REPOSITORY:-https://github.com/olmox001/NX-SDK.git}"
SDK_REF="${NEXS_SDK_REF:-main}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
INSTALL_PREFIX="${NEXS_SDK_INSTALL_PREFIX:-$DATA_HOME/nexsos/sdk}"
CACHE_ROOT="${NEXS_SDK_CACHE_ROOT:-$USERLAND_ROOT/.nxsdk}"
CACHE_SOURCE="$CACHE_ROOT/source"

valid_sdk() {
  local root="$1"
  [ -n "$root" ] || return 1
  [ -f "$root/config/toolchain-versions.env" ] || return 1
  [ -f "$root/sdk.mk" ] || return 1
  [ -d "$root/include/api" ] || return 1
  [ -d "$root/include/contract" ] || return 1
}

integrated_sdk() {
  local super top control sdk
  super="$(git -C "$USERLAND_ROOT" rev-parse --show-superproject-working-tree 2>/dev/null || true)"
  if [ -z "$super" ]; then
    top="$(git -C "$USERLAND_ROOT" rev-parse --show-toplevel 2>/dev/null || true)"
    if [ -n "$top" ] && [ "$top" != "$USERLAND_ROOT" ]; then super="$top"; fi
  fi
  [ -n "$super" ] || return 1
  control="$super/tools/setup-dependencies.sh"
  [ -x "$control" ] || return 1
  sdk="$($control --locate system 2>/dev/null)" || return 1
  valid_sdk "$sdk" || return 1
  printf '%s\n' "$sdk"
}

find_sdk() {
  local candidate
  if [ -n "${NEXS_SDK_ROOT:-}" ] && valid_sdk "$NEXS_SDK_ROOT"; then (cd "$NEXS_SDK_ROOT" && pwd); return 0; fi
  if candidate="$(integrated_sdk 2>/dev/null)"; then printf '%s\n' "$candidate"; return 0; fi
  for candidate in "$INSTALL_PREFIX/current" "$CACHE_SOURCE"; do
    if valid_sdk "$candidate"; then (cd "$candidate" && pwd); return 0; fi
  done
  return 1
}

ensure_sdk() {
  local found
  if found="$(find_sdk 2>/dev/null)"; then printf '%s\n' "$found"; return 0; fi
  command -v git >/dev/null 2>&1 || { echo "NexsOS userland: git is required to bootstrap NX-SDK" >&2; return 1; }
  mkdir -p "$CACHE_ROOT"
  if [ ! -d "$CACHE_SOURCE/.git" ]; then git clone "$SDK_REPOSITORY" "$CACHE_SOURCE"; fi
  git -C "$CACHE_SOURCE" fetch origin "$SDK_REF"
  git -C "$CACHE_SOURCE" checkout --detach FETCH_HEAD
  valid_sdk "$CACHE_SOURCE" || { echo "NexsOS userland: fetched NX-SDK is incomplete" >&2; return 1; }
  (cd "$CACHE_SOURCE" && pwd)
}

case "$MODE" in
  --print) find_sdk ;;
  --ensure) ensure_sdk ;;
  --check) root="$(find_sdk)" || { echo "NexsOS userland: NX-SDK not found" >&2; exit 1; }; printf 'NexsOS userland SDK: %s\n' "$root" ;;
  *) echo "usage: $0 [--print|--ensure|--check]" >&2; exit 2 ;;
esac
