#!/bin/sh
# scripts/check-layering.sh — ASTRA layering gate (Phase 10a, directive 3).
#
# spatch is used as a CHECKER, not a rewriter: the semantic patches delete every
# forbidden #include, so a non-empty diff means "kernel and userland are still
# entangled here".  Nothing is written back (no --in-place), so this is
# side-effect free.
#
# TWO passes, because the two directions are only violations in one tree each:
# `#include <os1.h>` is wrong in kernel/ and exactly right in user/.  The
# direction is the whole point, so it is encoded in WHAT gets scanned.
#
#   ./scripts/check-layering.sh            report, exit 1 if any violation
#   ./scripts/check-layering.sh --count    print the two counts only
#
# The baseline is NOT zero: Phase 10a is the phase that drives it there.
# Recording the figure is the point — it turns "the layering is bad" into a
# number that either goes down or does not.
set -eu
cd "$(dirname "$0")/.."

command -v spatch >/dev/null 2>&1 || {
  echo "check-layering: spatch (coccinelle) not installed — gate SKIPPED" >&2
  exit 0
}

[ -f scripts/layering-kernel.cocci ] || python3 scripts/gen-layering-cocci.py >/dev/null

# Vendored trees carry OTHER platforms' kernel headers (psp2/kernel, Haiku's
# kernel/OS.h) and their own libc headers; they are not this boundary.
FIRSTPARTY_USER=$(find user/sys/lib/lib.c user/sys/lib/execsvc_client.c \
                       user/sys/bin user/bin -maxdepth 1 -name '*.c' -o \
                       -path 'user/sys/bin/*' -name '*.h' 2>/dev/null |
                  grep -v -e '/sdl/' -e '/lua/' -e '/doom/' -e '/busybox/' \
                          -e '/base-nexs/' -e '/direct3d/' -e '/kilo/' || true)
KERNEL_SRC=$(find kernel -name '*.c' -o -name '*.h')

scan() { # $1 = cocci file, $2... = sources
  cocci=$1; shift
  printf '%s\n' "$@" | tr ' ' '\n' | grep -v '^$' |
    xargs spatch --very-quiet --no-includes --sp-file "$cocci" 2>/dev/null || true
}

DK=$(scan scripts/layering-kernel.cocci $KERNEL_SRC)
DU=$(scan scripts/layering-user.cocci $FIRSTPARTY_USER)

# NET count: coccinelle's pretty-printer relocates a trailing comment onto the
# preceding line, so removing ONE include can emit two '-' lines and one '+'.
# Counting '-' alone over-reports; subtracting the re-added '+' lines gives the
# number of includes actually deleted.
netcount() {
  minus=$(printf '%s\n' "$1" | grep -c '^-#include' 2>/dev/null | head -1)
  plus=$(printf '%s\n' "$1" | grep -c '^+#include' 2>/dev/null | head -1)
  : "${minus:=0}" "${plus:=0}"
  echo $((minus - plus))
}
NK=$(netcount "$DK")
NU=$(netcount "$DU")

if [ "${1:-}" = "--count" ]; then
  echo "kernel->userland: $NK   userland->kernel: $NU"
  exit 0
fi

echo "=== kernel/ including USERLAND headers: $NK ==="
printf '%s\n' "$DK" | grep -E '^(---|[-+]#include)' | grep -v '^+++' | sed 's/^/  /'
echo
echo "=== user/ including KERNEL code: $NU ==="
printf '%s\n' "$DU" | grep -E '^(---|[-+]#include)' | grep -v '^+++' | sed 's/^/  /'

[ "$NK" -eq 0 ] && [ "$NU" -eq 0 ] && { echo; echo "check-layering: OK"; exit 0; }
exit 1
