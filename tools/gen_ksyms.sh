#!/usr/bin/env bash
#
# tools/gen_ksyms.sh <nm> <kernel-pass1.elf> <out.S>
# ------------------------------------------------------------------------------
# Generate the in-kernel symbol table (.ksyms section) from the first-pass
# link, kallsyms-style (Phase A step 12; consumed by kernel/lib/backtrace.c).
#
# Only text symbols (T/t/W/w) enter the table.  The blob is ALLOC ("a") and is
# placed inside .rodata by both kernel.ld scripts, so it survives the aarch64
# `objcopy -O binary` — text addresses do not move between the two passes
# because .ksyms lands after .text in the layout.
#
# Blob layout (native endianness, see backtrace.c):
#   u64 count; u64 addrs[count]; u32 name_offs[count]; char names[];
# ------------------------------------------------------------------------------
set -euo pipefail

NM="$1"; ELF="$2"; OUT="$3"

"$NM" -n "$ELF" | awk '
  $3 != "" && ($2 == "T" || $2 == "t" || $2 == "W" || $2 == "w") {
    addr[n] = $1; name[n] = $3; n++;
  }
  END {
    print ".section .ksyms, \"a\"";
    print ".balign 8";
    print ".quad " n;
    for (i = 0; i < n; i++) print ".quad 0x" addr[i];
    off = 0;
    for (i = 0; i < n; i++) { print ".long " off; off += length(name[i]) + 1; }
    for (i = 0; i < n; i++) print ".asciz \"" name[i] "\"";
  }' > "$OUT"
