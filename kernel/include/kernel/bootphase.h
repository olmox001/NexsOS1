#ifndef _KERNEL_BOOTPHASE_H
#define _KERNEL_BOOTPHASE_H

/* Boot-phase tracking (S-ALIGN F9) — the K1/K2/K3 boot model made explicit:
 *   K1  memory/hardware bring-up (CPU, IRQ, timer, PMM/VMM, SMP)
 *   K2  kernel subsystem init (bus/block/GPU/VFS/registry/sched/compositor)
 *   K3  userland bring-up (PID1) — enters ONLY after K1+K2 are confirmed
 *   RUN steady state
 * The phase is single-writer (the BSP during kernel_main) and read by
 * panic() to stamp fault reports and scope failure policy: a K3-only
 * failure (userland won't load) must not take down a healthy K1+K2 —
 * the kernel stays alive and diagnosable (kernel-alone mode). */
enum boot_phase {
  BOOT_PHASE_K1_HW = 0,
  BOOT_PHASE_K2_SUBSYS,
  BOOT_PHASE_K3_USERLAND,
  BOOT_PHASE_RUNNING,
};

void boot_phase_set(enum boot_phase p);
enum boot_phase boot_phase_get(void);
const char *boot_phase_name(enum boot_phase p);

#endif /* _KERNEL_BOOTPHASE_H */
