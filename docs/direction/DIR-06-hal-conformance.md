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

## Method (per ASTRA / docs/nexs-astra-guidelines)
1. Define the contract (`*_ops`/`*_chip`) in `kernel/include/kernel/`.
2. Move the implementation next to its provider/driver, not under `kernel/arch/`.
3. Core calls only the contract; the ISA layer keeps only trap/context/MMU/TLB.
4. Prove it by building + booting BOTH arches with identical core behaviour.

## Acceptance
* No direct ISA register/MMIO access from `kernel/core`, `kernel/sched`, `kernel/mm`.
* IRQ EOI is a single chip contract on both arches; timer IRQ glue is ISA-entry-only.
* A shared-cause fault yields the SAME class of symptom on both arches (no divergence).
