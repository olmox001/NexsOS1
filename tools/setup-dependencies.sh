#!/usr/bin/env bash
# Reproducible external dependency bootstrap.
# Gitlink commits in the superproject are the authoritative versions.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:---init}"
MODULE_QUERY="${2:-}"
COREUTILS="$ROOT/user/bin/coreutils"

command -v git >/dev/null 2>&1 || {
  echo "[deps] ERROR: git is required." >&2
  exit 1
}
git -C "$ROOT" rev-parse --show-toplevel >/dev/null 2>&1 || {
  echo "[deps] ERROR: $ROOT is not inside a Git checkout." >&2
  exit 1
}
[ -f "$ROOT/.gitmodules" ] || {
  echo "[deps] ERROR: .gitmodules is missing." >&2
  exit 1
}

repair_coreutils_submodule_metadata() {
  # This fork was moved out of the monorepo, but its nested .gitmodules still
  # names gnulib by the old monorepo path. The actual gitlink is ./gnulib.
  # Repair only this metadata path; no Coreutils source files are changed.
  [ -f "$COREUTILS/.gitmodules" ] || return 0
  if git -C "$COREUTILS" ls-tree HEAD gnulib | grep -q '^160000 '; then
    local key="submodule.user/sys/lib/gnulib.path"
    local current
    current="$(git -C "$COREUTILS" config -f .gitmodules --get "$key" || true)"
    case "$current" in
      gnulib) ;;
      user/sys/lib/gnulib)
        echo "[deps] Repairing Coreutils nested gnulib path metadata."
        git -C "$COREUTILS" config -f .gitmodules "$key" gnulib
        ;;
      *)
        echo "[deps] ERROR: unexpected Coreutils gnulib path '$current'." >&2
        exit 1
        ;;
    esac
  fi
}

materialize_incomplete_submodule_checkouts() {
  # An interrupted previous bootstrap can leave a submodule HEAD at the pinned
  # gitlink while its index/worktree are still empty (Git then skips checkout
  # on the next update). Repair only this unmistakably incomplete state.
  git -C "$ROOT" submodule foreach --recursive '
    tracked_count=$(git ls-files | wc -l | tr -d " ")
    tree_count=$(git ls-tree -r HEAD | wc -l | tr -d " ")
    if [ "$tracked_count" -eq 0 ] && [ "$tree_count" -gt 0 ]; then
      echo "[deps] Completing interrupted checkout: $displaypath"
      git reset --hard HEAD
    fi
  '
}


locate_module() {
  local query="$1" key path
  [ -n "$query" ] || { echo "[deps] ERROR: --locate requires a submodule name or path." >&2; exit 2; }
  key="submodule.$query.path"
  path="$(git -C "$ROOT" config -f .gitmodules --get "$key" 2>/dev/null || true)"
  if [ -z "$path" ]; then
    path="$(git -C "$ROOT" config -f .gitmodules --get-regexp '^submodule\..*\.path$' 2>/dev/null | awk -v q="$query" '$2==q {print $2; exit}')"
  fi
  [ -n "$path" ] || { echo "[deps] ERROR: submodule not declared: $query" >&2; exit 1; }
  [ -e "$ROOT/$path/.git" ] || [ -d "$ROOT/$path/.git" ] || { echo "[deps] ERROR: submodule is not initialized: $path" >&2; exit 1; }
  (cd "$ROOT/$path" && pwd)
}

verify_materialized_submodule_checkouts() {
  git -C "$ROOT" submodule foreach --recursive '
    tracked_count=$(git ls-files | wc -l | tr -d " ")
    tree_count=$(git ls-tree -r HEAD | wc -l | tr -d " ")
    if [ "$tracked_count" -eq 0 ] && [ "$tree_count" -gt 0 ]; then
      echo "[deps] ERROR: submodule checkout is incomplete: $displaypath" >&2
      exit 1
    fi
  '
}

case "$MODE" in
  --locate)
    locate_module "$MODULE_QUERY"
    exit 0
    ;;
  --init)
    echo "[deps] Synchronizing top-level submodule URLs..."
    git -C "$ROOT" submodule sync
    echo "[deps] Checking out top-level dependencies at exact gitlink revisions..."
    git -C "$ROOT" submodule update --init --checkout
    repair_coreutils_submodule_metadata
    echo "[deps] Synchronizing and checking out recursive dependencies..."
    git -C "$ROOT" submodule sync --recursive
    git -C "$ROOT" submodule update --init --recursive --checkout
    materialize_incomplete_submodule_checkouts
    verify_materialized_submodule_checkouts
    ;;
  --check)
    if [ -f "$COREUTILS/.gitmodules" ] && git -C "$COREUTILS" ls-tree HEAD gnulib | grep -q '^160000 '; then
      current="$(git -C "$COREUTILS" config -f .gitmodules --get submodule.user/sys/lib/gnulib.path || true)"
      [ "$current" = "gnulib" ] || {
        echo "[deps] ERROR: Coreutils nested gnulib metadata is not repaired." >&2
        exit 1
      }
    fi
    verify_materialized_submodule_checkouts
    ;;
  *)
    echo "Usage: $0 [--init|--check|--locate NAME_OR_PATH]" >&2
    exit 2
    ;;
esac

STATUS="$(git -C "$ROOT" submodule status --recursive)"
printf '%s\n' "$STATUS"
if printf '%s\n' "$STATUS" | grep -Eq '^[+-U]'; then
  echo "[deps] ERROR: one or more submodules are uninitialized, divergent, or conflicted." >&2
  echo "[deps] Run: tools/setup-dependencies.sh --init" >&2
  exit 1
fi

echo "[deps] All submodules match the revisions pinned by this checkout."
