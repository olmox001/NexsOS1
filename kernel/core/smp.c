/*
 * kernel/core/smp.c
 * Arch-neutral secondary-CPU bring-up (S-ALIGN F8 — first slice of B6 #96).
 *
 * Both arch_smp_init() loops (PSCI on aarch64, INIT-SIPI on amd64) used to
 * implement the same logical step independently: "publish the CPU's idle task,
 * wake it through the arch contract, wait for its boot ack".  The SMP-IDLE-RACE
 * fix (#169/#170, commit 74fc5cc) had to be applied twice — once per arch —
 * because no shared primitive existed.  This module IS that primitive: the
 * ordering invariant and the ack handshake live here, once.
 *
 * The ack wait is bounded by wall time through the HAL timer contract
 * (arch_timer_get_count/_get_freq — the DIR-06 reference seam, same pattern as
 * the panic watchdog in printk.c), replacing the per-arch nop-count/udelay
 * divergence.  The handshake variable is acquire/release-paired with
 * smp_ack_boot() so the secondary's per-CPU initialization writes are visible
 * to the BSP before the ack is (S-ALIGN F3).
 */
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/types.h>

/* The boot-ack handshake cell.  Written ONLY through smp_ack_boot() by the
 * secondary, read ONLY by smp_bringup_secondary() on the BSP. */
static volatile uint32_t cpu_boot_ack;

/* smp_ack_boot - the secondary CPU announces itself online (called from
 * kernel_secondary_main after its per-CPU init).  Release store: everything
 * the secondary initialized must be visible before the BSP sees the ack. */
void smp_ack_boot(uint32_t cpu) {
  __atomic_store_n(&cpu_boot_ack, cpu, __ATOMIC_RELEASE);
}

/*
 * smp_bringup_secondary - bring one secondary CPU online.
 *
 * The one place that encodes the bring-up invariant (SMP-IDLE-RACE #169/#170):
 * the CPU's idle task is published BEFORE the wake, because the secondary
 * enables IRQs in kernel_secondary_main and can be preempted into schedule()
 * — which picks cpu_data[cpu].idle_task — before the BSP would otherwise
 * create it (NULL idle task -> context switch into NULL).
 *
 * cpu:       logical CPU id to start.
 * entry:     arch entry point handed to arch_cpu_wake_secondary (PSCI target /
 *            SIPI trampoline).
 * stack_top: top of the CPU's kernel stack (stacks descend), passed through
 *            the arch wake contract.
 *
 * Returns 0 when the CPU acked, -1 if the arch wake call itself failed
 * (firmware error / CPU absent), -2 on ack timeout (absent CPU or a very
 * slow hypervisor — its idle task already exists, so a late-arriving CPU
 * still schedules safely).  Failure POLICY (skip vs. abort the loop) stays
 * with the arch caller: only the mechanism is shared.
 */
int smp_bringup_secondary(uint32_t cpu, void (*entry)(void), void *stack_top) {
  /* Fresh handshake for this CPU (uniform: amd64 always reset, aarch64
   * relied on ids never repeating — resetting is correct for both). */
  __atomic_store_n(&cpu_boot_ack, 0u, __ATOMIC_RELAXED);

  /* 1. Idle task BEFORE the wake — the single-sourced #169/#170 invariant. */
  smp_create_idle_task(cpu);

  /* 2. Wake through the arch contract. */
  if (arch_cpu_wake_secondary(cpu, entry, stack_top) != 0)
    return -1;

  /* 3. Bounded acquire-wait for the ack (~10 s wall time, generous for a
   * slow hypervisor; the watchdog in printk.c uses the same timer seam). */
  uint64_t freq = arch_timer_get_freq();
  uint64_t deadline = arch_timer_get_count() + freq * 10ULL;
  while (__atomic_load_n(&cpu_boot_ack, __ATOMIC_ACQUIRE) != cpu) {
    if ((int64_t)(deadline - arch_timer_get_count()) <= 0)
      return -2;
    arch_nop();
  }
  return 0;
}
