/*
 * include/api/caps.h
 * Privilege levels and capability bits — the single source of truth shared
 * by the kernel (kernel/include/kernel/sched.h) and userland (os1.h).
 * USR-SEC-03 #79.
 *
 * A process has one PRIVILEGE LEVEL (the root of access control and the
 * resolver for the future multi-user model) and a CAPABILITY MASK (the
 * fine-grained authority enforced at the syscall boundary).
 */
#ifndef NEXS_API_CAPS_H
#define NEXS_API_CAPS_H

/* Privilege levels — lower number = more privilege.  PLVL_MACHINE is the
 * machine's own identity (kernel/init/services): NOT a login user, unkillable,
 * bypasses every capability check.  Real users slot in at root/user/guest. */
#define PLVL_MACHINE 0
#define PLVL_ROOT    1
#define PLVL_USER    2
#define PLVL_GUEST   3
#define PLVL_COUNT   4

/* Fine-grained capabilities — one per gated syscall surface. */
#define CAP_SPAWN     (1u << 0) /* SYS_SPAWN / spawn_level / spawn_caps */
#define CAP_FS_WRITE  (1u << 1) /* SYS_FILE_WRITE + open-for-write      */
#define CAP_IPC_ANY   (1u << 2) /* SYS_SEND to non-relatives           */
#define CAP_WINDOW    (1u << 3) /* SYS_CREATE_WINDOW + SET_FOCUS(self) */
#define CAP_REG_WRITE (1u << 4) /* SYS_REGISTRY write op               */
#define CAP_ALL \
  (CAP_SPAWN | CAP_FS_WRITE | CAP_IPC_ANY | CAP_WINDOW | CAP_REG_WRITE)

#endif /* NEXS_API_CAPS_H */
