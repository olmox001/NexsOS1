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

/* Spawn-mode flags (arg3 of SYS_SPAWN / SYS_SPAWN_CAPS) — the nxexec model
 * (#193): a DETACHED child does NOT inherit the spawner as its controlling
 * terminal (ctty_win stays -1).  For launchers (nxlauncher): a windowless
 * child spawned detached fails closed (no output surface) instead of writing
 * into the launcher's soon-minimized window as if it were a shell.  Windowed
 * children are unaffected (own-window-first stdout resolution). */
#define SPAWN_FLAG_DETACHED (1u << 0)
/* Every bit outside SPAWN_FLAGS_ALL is REJECTED kernel-side with -EINVAL
 * (fail closed): the spawn surface is the front door of process execution
 * and must never silently accept unknown semantics (#193 hardening). */
#define SPAWN_FLAGS_ALL SPAWN_FLAG_DETACHED

#endif /* NEXS_API_CAPS_H */
