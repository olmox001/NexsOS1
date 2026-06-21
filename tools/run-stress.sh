#!/usr/bin/env bash
# tools/run-stress.sh — launch one long-duration stress run on one arch, headless,
# capture the serial log, then score it with analyze_drift.py (perf brief §5/§6).
#
# Usage:
#   tools/run-stress.sh <aarch64|amd64> [DURATION_S] [extra /bin/stress args...]
# Examples:
#   tools/run-stress.sh aarch64 7200            # 2h aarch64 (ASID active, TCG)
#   tools/run-stress.sh amd64   7200            # 2h amd64  (PCID active, HVF)
#   tools/run-stress.sh aarch64 1800 --spawn    # 30min, spawn lane only
#
# Run "first one then the other" as requested — each call is self-contained.
# amd64 uses '-accel hvf -cpu host' so the PCID-tagged path is exercised (the
# host CPU exposes PCID); aarch64 uses TCG (ASID works there).  Build first:
#   make all ARCH=<arch>   (+ make build/aarch64/virt.dtb for aarch64)
set -u
ARCH="${1:-aarch64}"
DUR="${2:-7200}"
shift 2 2>/dev/null || shift $# 2>/dev/null
EXTRA="$*"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
mkdir -p logs
TS="$(date +%Y%m%d-%H%M%S)"
LOG="logs/stress-${ARCH}-${TS}.log"
SOCK="/tmp/qmp-stress-${ARCH}.sock"
rm -f "$LOG" "$SOCK"

echo "[run-stress] arch=$ARCH dur=${DUR}s extra='${EXTRA}' log=$LOG"

if [ "$ARCH" = "amd64" ]; then
  ACCEL="${ACCEL:-hvf}"   # HVF exposes PCID on this Intel host; ACCEL=tcg to force fallback
  qemu-system-x86_64 -accel "$ACCEL" -cpu host -m 5G -smp 4 \
    -serial file:"$LOG" -display none -qmp unix:"$SOCK",server,nowait \
    -device virtio-gpu-pci,disable-legacy=on,disable-modern=off \
    -device virtio-keyboard-pci,disable-legacy=on,disable-modern=off \
    -device virtio-mouse-pci,disable-legacy=on,disable-modern=off \
    -drive if=none,file=build/amd64/disk.img,id=hd0,format=raw \
    -device virtio-blk-pci,drive=hd0,disable-legacy=on,disable-modern=off \
    -kernel build/amd64/kernel.elf &
else
  qemu-system-aarch64 -M virt -cpu cortex-a57 -m 5G -smp 4 \
    -serial file:"$LOG" -display none -qmp unix:"$SOCK",server,nowait \
    -device virtio-gpu-device -device virtio-keyboard-device -device virtio-mouse-device \
    -drive if=none,file=build/aarch64/disk.img,id=hd0,format=raw \
    -device virtio-blk-device,drive=hd0 \
    -dtb build/aarch64/virt.dtb -kernel build/aarch64/kernel.bin &
fi
QPID=$!
trap 'kill $QPID 2>/dev/null' EXIT INT TERM

echo "[run-stress] booting (waiting 25s) ..."
sleep 25
# nxmemstat (ROOT /sys/bin service) does the privileged stats CSV logging AND
# spawns the USER load driver (/bin/stress) via --run, because OS1_sys_stats is
# ROOT-gated and the foreground-only shell can't co-run two long commands.
echo "[run-stress] launching: nxmemstat --log 60 --run /bin/stress --dur $DUR $EXTRA"
python3 tools/qmp_type.py "$SOCK" "nxmemstat --log 60 --run /bin/stress --dur ${DUR} ${EXTRA}"$'\n' 1 || {
  echo "[run-stress] qmp_type failed — is the guest at a shell prompt?"; }

# wait the run duration + a margin for boot/teardown
WAIT=$((DUR + 150))
echo "[run-stress] running for ~${DUR}s (waiting ${WAIT}s; tail -f $LOG to watch) ..."
sleep "$WAIT"

kill $QPID 2>/dev/null; wait $QPID 2>/dev/null; trap - EXIT INT TERM

echo ""
echo "===================== RESULTS ($ARCH) ====================="
echo "-- health (PANIC/NESTED/Unhandled count, expect 0) --"
grep -acE 'PANIC|NESTED|Unhandled' "$LOG"
echo "-- PCID/boot --"; grep -aE "PCID-tagged TLB|Boot Complete" "$LOG" | head -2
echo "-- stress summary --"; grep -aE "\[nxmemstat\]|\[stress\] (start|DONE)" "$LOG"
echo "-- drift analysis (warmup=1 drops boot transient) --"
python3 tools/analyze_drift.py "$LOG" --warmup 1
echo "Full serial log: $LOG"
