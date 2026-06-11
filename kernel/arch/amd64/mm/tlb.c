/*
 * kernel/arch/amd64/mm/tlb.c
 * SMP TLB shootdown via fixed-vector LAPIC IPI — amd64
 *
 * Resolves MM-VMM-05 / AMMU-08 on this arch.  x86 has no hardware TLB
 * broadcast (unlike AArch64's inner-shareable TLBI, which the DVM carries to
 * every PE): invlpg and CR3 reloads only affect the executing CPU, so after
 * an unmap/permission change every peer CPU may keep using a stale
 * translation — a correctness and isolation hole once the backing frame is
 * recycled.
 *
 * Protocol (one round at a time, serialised by tlb_shoot_lock):
 *   1. The initiator flushes locally, then publishes the set of online peer
 *      CPUs in tlb_shoot_waiting (one bit per cpu_id) and broadcasts the
 *      fixed IPI vector TLB_IPI_VECTOR (0xFD, all-excluding-self shorthand).
 *   2. Each peer's IPI handler performs a FULL local flush (CR3 reload) and
 *      clears its bit.  The handler flushes unconditionally — even when its
 *      bit was already force-cleared by an initiator timeout — so a late IPI
 *      can never be a missed flush.  A full flush (rather than a per-VA
 *      invlpg) sidesteps the race where a late handler reads the address of
 *      a NEWER round and leaves the old VA stale; on a 4-CPU QEMU guest the
 *      precision loss is noise.
 *   3. The initiator spins (bounded) until all bits clear.  A peer running
 *      with IRQs masked acknowledges late: the IPI stays pending in its
 *      LAPIC IRR and the flush happens the moment it unmasks, so a timeout
 *      here only means the ack was not OBSERVED in time, not that the peer
 *      will skip the flush.
 *
 * Deadlock safety: a CPU that wants to start its own round while another is
 * in flight spins on tlb_shoot_lock with IRQs possibly disabled (callers may
 * hold mm_lock irqsave); the spin loop services the in-flight round inline
 * (tlb_ipi_service), so the current initiator never waits forever on a CPU
 * that is itself waiting for the lock.
 *
 * Perf note: callers that unmap N pages in a loop currently trigger N
 * rounds (N×(ncpu-1) IPIs + full flushes).  Acceptable for today's rare
 * unmap paths (sbrk shrink, teardown does ONE _all round); if profiling ever
 * flags it, add a range/batch API on the same protocol.
 */
#include <arch/amd64/apic.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

#define TLB_IPI_VECTOR 0xFD

/* TLB_ACK_TIMEOUT_SPINS: pause-loop iterations before the initiator stops
 * waiting for stragglers.  Generous on purpose: under QEMU TCG a vCPU can be
 * de-scheduled by the host for milliseconds. */
#define TLB_ACK_TIMEOUT_SPINS 50000000UL

/* tlb_shoot_waiting: bitmask (1 << cpu_id) of peers that still owe an ack
 * for the in-flight round.  Written by the initiator under tlb_shoot_lock;
 * bits cleared by peers with an atomic AND. */
static volatile uint64_t tlb_shoot_waiting;
static DEFINE_SPINLOCK(tlb_shoot_lock);

/* tlb_ipi_ready: set once the handler is registered; before that (early
 * boot, single CPU running) shootdowns degrade to the local flush. */
static int tlb_ipi_ready;

/*
 * tlb_ipi_service - service the in-flight round if it targets this CPU.
 *
 * Called from the lock-acquisition spin in tlb_shootdown(): a CPU waiting to
 * start its own round may have IRQs disabled (caller holds mm_lock irqsave),
 * so the IPI could not be delivered — without this inline service the
 * current initiator would wait for our ack until timeout.
 */
static void tlb_ipi_service(void) {
  uint64_t bit = 1UL << hal_cpu_id();
  if (tlb_shoot_waiting & bit) {
    arch_impl_tlb_flush_local();
    __sync_fetch_and_and(&tlb_shoot_waiting, ~bit);
  }
}

/*
 * tlb_ipi_handler - IPI entry (vector 0xFD) on a peer CPU.
 *
 * Flushes unconditionally (see header: a late IPI after an initiator
 * timeout must still flush), then clears this CPU's ack bit.
 * EOI is chip-owned: idt.c calls irq_chip_end(vec) → lapic_eoi().
 */
static void tlb_ipi_handler(uint32_t irq, void *data) {
  (void)irq;
  (void)data;
  arch_impl_tlb_flush_local();
  __sync_fetch_and_and(&tlb_shoot_waiting, ~(1UL << hal_cpu_id()));
}

/*
 * amd64_tlb_ipi_init - register the shootdown IPI handler.
 *
 * Called once by the BSP from arch_vmm_init_hw(); before this, shootdowns
 * are local-only, which is correct because no AP is online yet.
 * pic_chip_enable() ignores vectors >= 48, so registering does not touch the
 * 8259; delivery is pure LAPIC fixed-vector.
 */
void amd64_tlb_ipi_init(void) {
  if (irq_register(TLB_IPI_VECTOR, tlb_ipi_handler, NULL) == 0) {
    tlb_ipi_ready = 1;
    pr_info("AMD64 TLB: shootdown IPI registered on vector 0x%x\n",
            TLB_IPI_VECTOR);
  } else {
    pr_err("%s", "AMD64 TLB: failed to register shootdown IPI vector\n");
  }
}

/*
 * tlb_shootdown - run one shootdown round.
 *
 * @va: target virtual address, or 0 for a full address-space flush.
 *      (VA 0 is never a mapped page — the NULL page is deliberately
 *      unmapped — so the overload is unambiguous.)
 *
 * The local flush always happens, even before SMP is up.
 */
static void tlb_shootdown(uintptr_t va) {
  /* Local CPU first. */
  if (va)
    arch_impl_tlb_flush_va(va);
  else
    arch_impl_tlb_flush_local();

  if (!tlb_ipi_ready)
    return;

  /* Snapshot the online peers.  cpu_id is the LAPIC ID (< MAX_CPUS = 64,
   * matching the width of tlb_shoot_waiting). */
  uint64_t mask = 0;
  uint32_t me = hal_cpu_id();
  for (uint32_t i = 0; i < MAX_CPUS && i < 64; i++) {
    if (i != me && cpu_data[i].online)
      mask |= 1UL << i;
  }
  if (!mask)
    return;

  /* Serialise rounds; service the in-flight one while waiting (deadlock
   * safety — see file header). */
  uint64_t flags;
  while (!spin_trylock_irqsave(&tlb_shoot_lock, &flags)) {
    tlb_ipi_service();
    __asm__ __volatile__("pause");
  }

  tlb_shoot_waiting = mask;
  arch_wmb();
  lapic_send_ipi(0, ICR_FIXED | ICR_ASSERT | ICR_ALL_EXCL_SELF |
                        TLB_IPI_VECTOR);

  uint64_t spins = 0;
  while (tlb_shoot_waiting != 0) {
    __asm__ __volatile__("pause");
    if (++spins == TLB_ACK_TIMEOUT_SPINS) {
      pr_warn("AMD64 TLB: shootdown ack timeout (waiting=0x%lx); peers will "
              "flush on IRQ unmask\n",
              tlb_shoot_waiting);
      tlb_shoot_waiting = 0;
      break;
    }
  }

  spin_unlock_irqrestore(&tlb_shoot_lock, flags);
}

/* amd64_tlb_shootdown_va - make a single VA's translation globally invisible. */
void amd64_tlb_shootdown_va(uintptr_t va) { tlb_shootdown(va); }

/* amd64_tlb_shootdown_all - flush the whole TLB on every online CPU. */
void amd64_tlb_shootdown_all(void) { tlb_shootdown(0); }
