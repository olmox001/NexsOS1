# NEXS-DIR-06 — ALL kernel code through the HAL (HAL-ARCH-01)

Aligns with the `docs/ASTRA.md` update: *"All abstractions of the ISA
implementation … pass through the HAL layer to uniformly abstract the kernel for
both architectures … All architectural implementations must be abstracted in the
HAL."* ISA layer rule (ASTRA §1.3): **minimal** — only context switch, trap entry,
page-table walks, TLB maintenance; everything else is a provider behind a contract.

## Why this is now a certified priority
TIMER-UAF-01 produced **two different symptoms per architecture** from **one**
root cause (a corrupted per-CPU timer node): a NULL+8 data-abort on aarch64, a
`#GP` on `ret` on amd64. Divergent symptoms for a shared bug are the signal that
the arch layer is still visible to core code in places it should not be. The
**timer is now the model to copy**: `kernel_timer_tick` and the per-CPU drift
accounting are fully HAL-driven (`timer_percpu_tick`/`timer_percpu_arm` over
`arch_timer_get_count/_get_freq/_set_compare`), arch-neutral, identical on both
arches. Bring the rest of the kernel to the same standard.

## Audit targets (non-conformities to migrate behind contracts)
* **IRQ EOI / chip path (amd64).** `pic_chip_end` performs BOTH a LAPIC EOI and a
  legacy-PIC EOI for vector 32; the timer EOI should be one uniform
  `irq_chip_end()` contract with no LAPIC-vs-PIC special-casing leaking up. Verify
  the aarch64 GIC path expresses the same contract.
* **Direct ISA access in core.** Sweep `kernel/core`, `kernel/sched`, `kernel/mm`
  for direct `rdmsr/wrmsr/outb/inb`, `cntvct_el0`/`cntfrq_el0`, CR-register, or
  GIC/LAPIC MMIO pokes that bypass a HAL primitive.
* **`platform.c`.** Frozen until B4 by ASTRA, but inventory what still lives there
  (e.g. `timer_get_us` dummy on amd64, ARCH-03) so the migration list is explicit.
* **Per-arch timer IRQ glue.** `idt.c` (amd64 vec 32) and the aarch64 timer driver
  should differ ONLY in the ISA trap entry; the accounting + scheduler tick is
  already shared — keep it that way and forbid regressions.

## Audit — first pass (results)
Swept `kernel/` for ISA leakage OUTSIDE `kernel/arch/`:
* **Core is clean.** No inline asm, no `rdmsr/outb/cntvct/CR`/LAPIC/GIC MMIO and
  no raw chip calls in `kernel/core`, `kernel/sched`, `kernel/mm`, `kernel/irq`.
* **IRQ EOI is already contracted — NOT a leak.** Core issues EOI via
  `irq_chip_end()` → `current_chip->end()`; `pic_chip_end` is the amd64 chip's
  *implementation* of that contract (it owns the full LAPIC+8259 sequence), and
  aarch64 supplies its own `->end`. The contract is uniform; only the providers
  differ, which is correct.
* **FIXED — `kernel/sched/elf.c` PTE encoding.** The ELF loader (arch-neutral
  core) hand-encoded per-arch PTE bits under `#ifdef ARCH_AARCH64/AMD64`. Now it
  selects among four arch-neutral VMM profiles — `PAGE_USER` (RW+X),
  `PAGE_USER_DATA` (RW), `PAGE_USER_RX` (RO+X text), `PAGE_USER_RO` (RO rodata) —
  added to `vmm.h` for both arches. No `#ifdef` in the loader; amd64 bits are
  identical, aarch64 gains the (correct) PXN hardening so the kernel can never
  execute a user page. Verified: both arches build + boot, all user ELFs
  (init/notify_srv/nxshell) execute, W^X intact, 0 PANIC.

Remaining (low priority, tracked here):
* `kernel/main.c` — `kernel_main` signature differs per arch (amd64 multiboot
  magic+mbi vs aarch64 x0/FDT). Legitimate boot-ABI difference; could be hidden
  behind a thin `arch_boot_args()` shim but is not a core-logic leak.
* `kernel/drivers/ps2/ps2.c` — PS/2 is x86-only hardware; the `#ifdef ARCH_AMD64`
  is a provider gate, acceptable. `virtio_input.c` ifdef to review.
* `platform.c` (`timer_get_us` dummy on amd64, ARCH-03) — frozen until B4.

## Method (per ASTRA / docs/nexs-astra-guidelines)
1. Define the contract (`*_ops`/`*_chip`) in `kernel/include/kernel/`.
2. Move the implementation next to its provider/driver, not under `kernel/arch/`.
3. Core calls only the contract; the ISA layer keeps only trap/context/MMU/TLB.
4. Prove it by building + booting BOTH arches with identical core behaviour.

## Acceptance
* No direct ISA register/MMIO access from `kernel/core`, `kernel/sched`, `kernel/mm`.
* IRQ EOI is a single chip contract on both arches; timer IRQ glue is ISA-entry-only.
* A shared-cause fault yields the SAME class of symptom on both arches (no divergence).
