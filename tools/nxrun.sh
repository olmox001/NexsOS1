#!/usr/bin/env bash
#
# nxrun.sh — headless boot, drive the guest shell, report a verdict.
#
# This is the verification loop that was previously retyped by hand every
# time, which is how its failure modes kept being rediscovered:
#
#   * The QMP socket path must be SHORT.  AF_UNIX caps sun_path at 104 bytes
#     and a scratchpad path exceeds it; the failure surfaces as a connect
#     error long after the boot looked fine, so the socket is always made
#     under a short mktemp dir here.
#   * A stray QEMU from an earlier run holds the write lock on disk.img and
#     silently blocks the next boot, so one is killed before each run.
#   * On aarch64 the DTB is a prerequisite of `make run`, not of `make all`.
#     Booting without it makes QEMU exit immediately and leaves an EMPTY
#     serial log, which reads exactly like a guest hang.  It is built here.
#
# Usage:
#   tools/nxrun.sh [-a arch] [-n runs] [-c 'cmd'] [-e name=N] ...
#
#   -a  amd64 | aarch64            (default: $ARCH, else aarch64)
#   -n  number of consecutive runs (default: 1)
#   -c  a shell command to type into the guest; repeatable, run in order
#   -e  pin a test's expected TOTAL, e.g. -e captest=64 -e libctest=18
#
# Exit status is non-zero if ANY run faulted, failed to boot, OR FAILED ITS
# TESTS, so it can gate a phase directly.  With no -c it just boots and checks
# for faults, which is the cheap smoke test; with -c captest -c libctest it is
# the full gate.
#
# A command whose first word ends in `test` is treated as a test and must
# produce a `<name>] done: N/M passed` line.  The gate is "N of N, with N > 0"
# and deliberately NOT "zero failures", because three different outcomes are
# bad and only one of them is "some cases failed":
#
#   * NO `done:` line at all is a FAILURE, not a pass.  A truncated qmp_type
#     command types a prefix and the guest just sits there; if silence counted
#     as success, the harness would certify the exact outcome it exists to
#     catch.  This is also why the wait below is on the EVIDENCE and not a
#     blind sleep — with silence now fatal, a sleep that is too short on TCG
#     would report a failure the guest never had.
#   * `done: 0/0 passed, 0 failure(s)` has zero failures and is still not a
#     pass: zero cases run is not a green run.  "0 failures" would accept it.
#   * N != M — some cases failed.  The count is REPORTED either way, so a
#     regression names its own number instead of just flipping a verdict.
#
# Known limit of reading the serial log, written down rather than left to be
# rediscovered: the log also contains the ECHO of what was typed into the
# guest.  A command whose text itself contained `<name>] done: N/M` would be
# counted as that test's result.  Nothing we run does — `captest` and
# `libctest` are typed bare — but the four failure paths above were verified
# by exploiting exactly this, so it is a property of the harness and not a
# theoretical one.
set -u

ARCH="${ARCH:-aarch64}"
RUNS=1
CMDS=()
EXPECT=()

while getopts "a:n:c:e:" o; do
  case "$o" in
    a) ARCH="$OPTARG" ;;
    n) RUNS="$OPTARG" ;;
    c) CMDS+=("$OPTARG") ;;
    e) EXPECT+=("$OPTARG") ;;
    *) sed -n '2,45p' "$0"; exit 2 ;;
  esac
done

# is_test <cmd> — a command is a test if its first word ends in `test`.  That
# keeps `-c ls` a free-form command while making `-c captest` a gated one,
# without a second flag to forget.
is_test() { case "${1%% *}" in *test) return 0 ;; *) return 1 ;; esac; }

# expected_total <name> — the pinned total from -e, or empty when unpinned.
# Unpinned still enforces N/N with N>0; pinning adds "and N is the number I
# expect", which is what catches a test binary that silently lost cases.
expected_total() {
  local e
  for e in ${EXPECT[@]+"${EXPECT[@]}"}; do
    case "$e" in "$1="*) echo "${e#*=}"; return ;; esac
  done
}

# done_lines <name> <log> — how many completion lines this test has printed so
# far.  Counting rather than matching lets the same test be run twice in one
# boot without the second wait returning instantly on the first one's line.
done_lines() {
  local n
  n=$(grep -c "$1\] done:" "$2" 2>/dev/null)
  echo "${n:-0}"
}

cd "$(dirname "$0")/.." || exit 1
ROOT="$PWD"
B="$ROOT/build/$ARCH"

# QEMU flags per arch.  amd64 uses virtio-PCI, aarch64 virtio-MMIO on the virt
# machine — the transports differ, so a message-path bug can reproduce on one
# and not the other.  Keep both honest by running both.
if [ "$ARCH" = "amd64" ]; then
  QEMU=qemu-system-x86_64
  MACHINE=(-m 5G -smp 4)
  DEVS=(-vga none
        -device virtio-gpu-pci,disable-legacy=on,disable-modern=off
        -device virtio-keyboard-pci,disable-legacy=on,disable-modern=off
        -device virtio-mouse-pci,disable-legacy=on,disable-modern=off
        -drive "if=none,file=$B/disk.img,id=hd0,format=raw"
        -device virtio-blk-pci,drive=hd0,disable-legacy=on,disable-modern=off
        -kernel "$B/kernel.elf")
  BOOT_WAIT=25
  TEST_WAIT=120
else
  QEMU=qemu-system-aarch64
  MACHINE=(-M virt -cpu cortex-a57 -m 5G -smp 4)
  make "ARCH=$ARCH" "$B/virt.dtb" >/dev/null 2>&1 || {
    echo "nxrun: cannot build $B/virt.dtb" >&2; exit 1; }
  DEVS=(-device virtio-gpu-device -device virtio-keyboard-device
        -device virtio-mouse-device
        -drive "if=none,file=$B/disk.img,id=hd0,format=raw"
        -device virtio-blk-device,drive=hd0
        -dtb "$B/virt.dtb" -kernel "$B/kernel.bin")
  BOOT_WAIT=45   # TCG emulation: everything takes longer here
  TEST_WAIT=300  # ditto, and a missing result is now fatal — be generous
fi

for f in "$B/disk.img"; do
  [ -f "$f" ] || { echo "nxrun: missing $f — build first" >&2; exit 1; }
done

rc=0
for run in $(seq 1 "$RUNS"); do
  pkill -f "$QEMU" 2>/dev/null; sleep 1
  D=$(mktemp -d /tmp/nxr.XXXX)   # short: AF_UNIX sun_path is 104 bytes

  env -u GTK_PATH -u GTK_EXE_PREFIX -u GTK_MODULES -u GTK_IM_MODULE_FILE \
      -u GIO_MODULE_DIR -u GSETTINGS_SCHEMA_DIR -u XDG_DATA_DIRS \
      -u XDG_DATA_HOME -u LOCPATH \
    "$QEMU" "${MACHINE[@]}" -serial "file:$D/ser.log" -display none \
      -qmp "unix:$D/qmp.sock,server,nowait" "${DEVS[@]}" \
      >/dev/null 2>"$D/qemu.err" &

  # Wait for userland, don't guess at it.
  booted=0
  for _ in $(seq 1 "$((BOOT_WAIT * 2))"); do
    if grep -q "Entering supervisor loop" "$D/ser.log" 2>/dev/null; then
      booted=1; break
    fi
    sleep 1
  done

  if [ "$booted" = 0 ]; then
    echo "run$run/$ARCH: NO BOOT — $(head -c 200 "$D/qemu.err" 2>/dev/null)"
    echo "  log: $D/ser.log"
    rc=1
    continue
  fi

  for c in ${CMDS[@]+"${CMDS[@]}"}; do
    name=${c%% *}
    before=$(done_lines "$name" "$D/ser.log")

    # $'...' matters: QCODE maps a REAL newline to "ret"; a literal backslash-n
    # raises KeyError and types a TRUNCATED command, which looks like a hang.
    python3 tools/qmp_type.py "$D/qmp.sock" "$c
" 2 >/dev/null 2>&1

    if is_test "$c"; then
      # Wait for the evidence, not for the clock.  Breaking as soon as the
      # line appears also makes a passing run finish in its own time instead
      # of the worst case.
      for _ in $(seq 1 "$TEST_WAIT"); do
        [ "$(done_lines "$name" "$D/ser.log")" -gt "$before" ] && break
        sleep 1
      done
    else
      sleep 20
    fi
  done

  # Match the BANNERS the kernel actually prints, not the words.  A
  # case-insensitive search for "panic" also matches ordinary log text that
  # merely contains it -- a diagnostic reading "a panic will not stop the other
  # cores" made this report a fault on a healthy boot.  A gate that cries wolf
  # gets ignored, so the patterns are anchored to the real output:
  #   "*** KERNEL PANIC", "KERNEL PAGE FAULT:", "KERNEL GENERAL PROTECTION FAULT"
  faults=$(grep -cE "KERNEL PANIC|KERNEL PAGE FAULT|GENERAL PROTECTION FAULT|Unrecoverable kernel" "$D/ser.log")
  heap=$(grep -c "Invalid magic" "$D/ser.log")

  verdict=ok
  why=""
  if [ "$faults" -gt 0 ] || [ "$heap" -gt 0 ]; then
    verdict=FAULT
    why="faults=$faults heap=$heap"
  fi

  # Test results are a GATE, not a printout.  See the header for why the rule
  # is "N of N, N>0" and why an ABSENT result fails as loudly as a failed one.
  results=""
  for c in ${CMDS[@]+"${CMDS[@]}"}; do
    is_test "$c" || continue
    name=${c%% *}
    line=$(grep -oE "$name\] done: [0-9]+/[0-9]+" "$D/ser.log" | tail -1)
    if [ -z "$line" ]; then
      results="$results $name=NO-RESULT"
      [ "$verdict" = FAULT ] || verdict=FAIL
      continue
    fi
    got=${line##*: }
    pass=${got%%/*}
    total=${got##*/}
    exp=$(expected_total "$name")
    results="$results $name=$got"
    if [ "$total" -eq 0 ] || [ "$pass" != "$total" ]; then
      [ "$verdict" = FAULT ] || verdict=FAIL
    elif [ -n "$exp" ] && [ "$total" != "$exp" ]; then
      results="$results(expected $exp)"
      [ "$verdict" = FAULT ] || verdict=FAIL
    fi
  done

  [ "$verdict" = ok ] || rc=1
  echo "run$run/$ARCH: $verdict${results:-} faults=$faults heap=$heap${why:+ ($why)}"
  [ "$verdict" = ok ] || echo "  log: $D/ser.log"
done

pkill -f "$QEMU" 2>/dev/null
exit $rc
