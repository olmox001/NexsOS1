#!/bin/sh
set -eu

arch="${1:-}"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel_root=$(CDPATH= cd -- "$root/.." && pwd)

case "$arch" in
  amd64)
    ld_tool="${AMD64_LD:-x86_64-elf-ld}"
    nm_tool="${AMD64_NM:-x86_64-elf-nm}"
    ;;
  aarch64)
    ld_tool="${AARCH64_LD:-aarch64-none-elf-ld}"
    nm_tool="${AARCH64_NM:-aarch64-none-elf-nm}"
    ;;
  *)
    echo "usage: $0 amd64|aarch64" >&2
    exit 2
    ;;
esac

archive="$root/build/$arch/libnxhal.a"
consolidated="$root/build/$arch/nxhal-boundary.o"
symbols="$root/build/$arch/nxhal-boundary.undefined"

if [ ! -f "$archive" ]; then
  echo "missing HAL archive: $archive" >&2
  exit 2
fi

"$ld_tool" -r --whole-archive "$archive" --no-whole-archive -o "$consolidated"
"$nm_tool" -u "$consolidated" |
  sed 's/^[[:space:]]*U[[:space:]]*//' |
  sort -u > "$symbols"

echo "NX-HAL $arch consolidated external symbols:"
cat "$symbols"

# These service families already have explicit HAL protocols. Any direct edge
# here is a layering regression, not accepted technical debt.
if grep -E '^(hal_device_|hal_register_device$|pci_|irq_(register|register_chip|enable|disable|chip_end)$|fault_(enter|exit|handle_user_or_panic)$|backtrace_(regs|scan)$|timer_percpu_(arm_current|tick_current)$|smp_bringup_secondary$|panic$|v?printk|pr_)' "$symbols"; then
  echo "NX-HAL $arch: migrated Core service edge reintroduced" >&2
  exit 1
fi

# NX-CORE may include HAL public headers, never provider/ISA headers or
# architecture-selection preprocessor branches.
if grep -RIn     --exclude-dir=HAL --exclude-dir=build     --include='*.c' --include='*.h'     -E '#[[:space:]]*include[[:space:]]*[<"]arch/|#[[:space:]]*(if|ifdef|ifndef).*ARCH_(AMD64|AARCH64)'     "$kernel_root" >/tmp/nxhal-generic-arch-leaks.$$; then
  cat /tmp/nxhal-generic-arch-leaks.$$ >&2
  rm -f /tmp/nxhal-generic-arch-leaks.$$
  echo "NX-CORE: provider/architecture leak detected" >&2
  exit 1
fi
rm -f /tmp/nxhal-generic-arch-leaks.$$

echo "PASS: NX-HAL $arch migrated-edge and generic-ISA boundary regression gate"
