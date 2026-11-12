#!/usr/bin/env bash
set -euo pipefail

# Keep REA isolated from the user's default Node installation. The explicit
# prefix avoids fnm/npm inheriting a different Node version's global prefix.
FNM_ROOT="${FNM_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/fnm}"
NODE_PREFIX="$FNM_ROOT/node-versions/v24.22.1/installation"
NODE="$NODE_PREFIX/bin/node"
REA_ENTRY="$NODE_PREFIX/lib/node_modules/rea-agents/scripts/rea.mjs"

if [[ ! -x "$NODE" || ! -f "$REA_ENTRY" ]]; then
  printf '%s\n' 'REA requires rea-agents@6.8.0 installed under fnm Node.js 24.22.1.' >&2
  printf 'Expected Node: %s\nExpected CLI: %s\n' "$NODE" "$REA_ENTRY" >&2
  printf '%s\n' 'Install with: env -u npm_config_prefix -u npm_config_global_prefix -u npm_config_globalconfig fnm exec --using=24.22.1 npm install -g --allow-scripts=frida rea-agents@6.8.0' >&2
  exit 127
fi

# Discover the optional analysis engines installed for this host without
# overwriting explicit caller choices. These variables are also injected into
# MCP clients by `rea setup`.
if [[ -z "${GHIDRA_INSTALL_DIR:-}" ]]; then
  candidate="$HOME/Applications/ghidra_12.1.4_PUBLIC"
  [[ -x "$candidate/support/analyzeHeadless" ]] && export GHIDRA_INSTALL_DIR="$candidate"
fi

if [[ -z "${JAVA_HOME:-}" ]] && [[ -x /usr/libexec/java_home ]]; then
  candidate="$(/usr/libexec/java_home -v 21 2>/dev/null || true)"
  [[ -n "$candidate" && -x "$candidate/bin/java" ]] && export JAVA_HOME="$candidate"
fi

if [[ -z "${HOPPER_LAUNCHER_PATH:-}" ]]; then
  candidate="$HOME/Applications/Hopper Disassembler.app/Contents/MacOS/hopper"
  [[ -x "$candidate" ]] && export HOPPER_LAUNCHER_PATH="$candidate"
fi

if [[ -z "${REA_KUNA_COMMAND:-}" ]]; then
  candidate="$HOME/Applications/NXOLMO-tools/kuna-v1.833/kuna-v1.833-macos-x86_64/kuna"
  [[ -x "$candidate" ]] && export REA_KUNA_COMMAND="$candidate"
fi

if [[ -z "${REA_JADX_MCP_JAR:-}" ]]; then
  candidate="$HOME/.local/share/rea/android/jadx-headless-mcp-0.7.1-all.jar"
  [[ -f "$candidate" ]] && export REA_JADX_MCP_JAR="$candidate"
fi

# Make the selected decompiler visible to REA-owned subprocesses as well as
# callers using the bundled Kuna skill.
if [[ -n "${REA_KUNA_COMMAND:-}" ]]; then
  export PATH="$(dirname "$REA_KUNA_COMMAND"):$PATH"
fi

exec "$NODE" "$REA_ENTRY" "$@"
