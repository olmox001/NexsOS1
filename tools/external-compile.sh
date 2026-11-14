#!/usr/bin/env bash
set -u

if [ "$#" -lt 3 ] || [ "$2" != "--" ]; then
  echo "usage: $0 LOG -- COMMAND [ARG ...]" >&2
  exit 2
fi

log="$1"
shift 2
mkdir -p "$(dirname "$log")"
tmp="$log.tmp.$$"
rm -f "$tmp"

"$@" 2>"$tmp"
status=$?

if [ "$status" -eq 0 ]; then
  if grep -q 'warning:' "$tmp"; then
    mv -f "$tmp" "$log"
  else
    rm -f "$tmp" "$log"
  fi
  exit 0
fi

cat "$tmp" >&2
mv -f "$tmp" "$log"
exit "$status"
