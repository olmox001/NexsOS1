#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# install_deps.sh — Dipendenze macOS per kernel_doctor.py
# Supporta: Intel (x86_64) e Apple Silicon (arm64)
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'
BLU='\033[0;34m'; CYN='\033[0;36m'; RST='\033[0m'; BLD='\033[1m'

banner() { echo -e "\n${BLU}${BLD}══ $* ══${RST}"; }
ok()     { echo -e "  ${GRN}✓${RST} $*"; }
warn()   { echo -e "  ${YLW}⚠${RST}  $*"; }
fail()   { echo -e "  ${RED}✗${RST} $*"; }
step()   { echo -e "  ${CYN}→${RST} $*"; }

# ── rilevamento architettura ──────────────────────────────────────────────────
ARCH="$(uname -m)"
if [[ "$ARCH" == "arm64" ]]; then
    BREW_PREFIX="/opt/homebrew"
    ok "Apple Silicon (arm64) — prefix: $BREW_PREFIX"
else
    BREW_PREFIX="/usr/local"
    ok "Intel x86_64 — prefix: $BREW_PREFIX"
fi

# ── brew check ────────────────────────────────────────────────────────────────
banner "Homebrew"
if ! command -v brew &>/dev/null; then
    fail "Homebrew non trovato. Installa da https://brew.sh"
    fail '/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
    exit 1
fi
ok "Homebrew $(brew --version | head -1)"

banner "Aggiornamento indice brew"
brew update --quiet && ok "indice aggiornato"

# ── funzione install sicura ───────────────────────────────────────────────────
install_formula() {
    local formula="$1"
    local label="${2:-$formula}"
    if brew list --formula "$formula" &>/dev/null 2>&1; then
        ok "$label già installato ($(brew list --versions "$formula"))"
    else
        step "Installo $label..."
        brew install "$formula" && ok "$label installato"
    fi
}

# ── llvm (clang-tidy, clang-format, clang static analyzer) ───────────────────
banner "LLVM toolchain (clang-tidy, clang-format, scan-build)"
install_formula llvm "LLVM"

LLVM_BIN="$BREW_PREFIX/opt/llvm/bin"
if [[ -d "$LLVM_BIN" ]]; then
    ok "clang-tidy:   $LLVM_BIN/clang-tidy"
    ok "clang-format: $LLVM_BIN/clang-format"
    ok "scan-build:   $LLVM_BIN/scan-build"
else
    warn "llvm bin non trovato in $LLVM_BIN"
fi

# ── coccinelle ────────────────────────────────────────────────────────────────
banner "Coccinelle (semantic patching)"
install_formula coccinelle "Coccinelle"
if command -v spatch &>/dev/null; then
    ok "spatch: $(spatch --version 2>&1 | head -1)"
else
    SPATCH_PATH="$BREW_PREFIX/bin/spatch"
    if [[ -f "$SPATCH_PATH" ]]; then
        ok "spatch trovato: $SPATCH_PATH"
    else
        warn "spatch non in PATH — aggiungi $BREW_PREFIX/bin al PATH"
    fi
fi

# ── python3 + dipendenze ──────────────────────────────────────────────────────
banner "Python 3"
if ! command -v python3 &>/dev/null; then
    install_formula python3 "Python 3"
fi
ok "Python $(python3 --version)"

# librerie opzionali per il report HTML
step "Verifico librerie Python..."
python3 -c "import json, re, os, sys, shutil, subprocess, argparse" 2>/dev/null \
    && ok "stdlib OK (json, re, os, subprocess, argparse)" \
    || fail "stdlib mancante — installazione Python anomala"

# ── bear (genera compile_commands.json per Makefile senza cmake) ──────────────
banner "Bear (compile_commands.json da Makefile)"
install_formula bear "Bear"
if command -v bear &>/dev/null; then
    ok "bear: $(bear --version 2>&1 | head -1)"
fi

# ── jq (debug/ispezione JSON) ─────────────────────────────────────────────────
banner "jq (debug JSON)"
install_formula jq "jq"

# ── PATH export suggerito ─────────────────────────────────────────────────────
banner "Configurazione PATH"

SHELL_RC=""
if [[ "$SHELL" == *"zsh"* ]];  then SHELL_RC="$HOME/.zshrc"; fi
if [[ "$SHELL" == *"bash"* ]]; then SHELL_RC="$HOME/.bashrc"; fi

LLVM_EXPORT="export PATH=\"$LLVM_BIN:\$PATH\""
BREW_EXPORT="export PATH=\"$BREW_PREFIX/bin:\$PATH\""

if [[ -n "$SHELL_RC" ]]; then
    if ! grep -q "$LLVM_BIN" "$SHELL_RC" 2>/dev/null; then
        echo "" >> "$SHELL_RC"
        echo "# LLVM toolchain — aggiunto da install_deps.sh" >> "$SHELL_RC"
        echo "$LLVM_EXPORT" >> "$SHELL_RC"
        ok "Aggiunto $LLVM_BIN a $SHELL_RC"
    else
        ok "$LLVM_BIN già in $SHELL_RC"
    fi
fi

echo ""
echo -e "${BLD}Per attivare subito:${RST}"
echo -e "  ${CYN}$LLVM_EXPORT${RST}"
echo -e "  ${CYN}source $SHELL_RC${RST}"

# ── riepilogo finale ──────────────────────────────────────────────────────────
banner "Riepilogo"

check_bin() {
    local bin="$1"
    local paths=("$LLVM_BIN/$bin" "$BREW_PREFIX/bin/$bin" "$(command -v "$bin" 2>/dev/null || true)")
    for p in "${paths[@]}"; do
        if [[ -x "$p" ]]; then
            ok "$bin → $p"
            return
        fi
    done
    warn "$bin non trovato"
}

check_bin clang-tidy
check_bin clang-format
check_bin spatch
check_bin bear
check_bin jq
check_bin python3

echo ""
echo -e "${GRN}${BLD}✅ Installazione completata.${RST}"
echo -e "   Ora puoi eseguire:"
echo -e "   ${CYN}python3 kernel_doctor.py --all /path/to/your/kernel${RST}"
echo ""
