/*
 * include/abi/caps.h
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
 * machine's own identity (kernel/nxinit/services): NOT a login user,
 * unkillable, bypasses every capability check.  Real users slot in at
 * root/user/guest. */
#define PLVL_MACHINE 0
#define PLVL_ROOT 1
#define PLVL_USER 2
#define PLVL_GUEST 3
#define PLVL_COUNT 4

/* Fine-grained capabilities — one per gated syscall surface. */
#define CAP_SPAWN (1u << 0)     /* SYS_SPAWN / spawn_level / spawn_caps */
#define CAP_FS_WRITE (1u << 1)  /* SYS_FILE_WRITE + open-for-write      */
#define CAP_IPC_ANY (1u << 2)   /* SYS_SEND to non-relatives           */
#define CAP_WINDOW (1u << 3)    /* SYS_CREATE_WINDOW + SET_FOCUS(self) */
#define CAP_REG_WRITE (1u << 4) /* SYS_REGISTRY write op               */
#define CAP_ALL                                                                \
  (CAP_SPAWN | CAP_FS_WRITE | CAP_IPC_ANY | CAP_WINDOW | CAP_REG_WRITE)

/*
 * caps_for_level - the DEFAULT capability mask a privilege level is created
 * with.  THE single source of truth for the level->mask policy, deliberately
 * here in the shared caps header rather than in the scheduler.
 *
 * It used to live as `level_ceiling[]` inside kernel/sched/process.c and was
 * MIRRORED by hand in userland (nxperm.h nxperm_level_mask()).  Two copies of
 * one policy in two trees is a latent drift bug — a level's authority would
 * silently disagree between what the kernel grants and what the permissions UI
 * claims.  Both now derive from this one function, which is also where a future
 * per-service manifest (ASTRA §7.11 Q5) layers its overrides.
 *
 * Semantics of the LEVELS themselves (the capability MASK is only half of it —
 * the other half is the per-namespace ACLs: the VFS write tree in
 * vfs_write_allowed(), the registry owner model, /sys/bin immutability):
 *   machine  full authority, bypasses every capability check (it IS the check).
 *   root     full capability mask; restricted only by the resource ACLs
 *            (cannot write /sys/bin or /system — that is the VFS ACL's job,
 *            not the mask's).
 *   user     full capability mask today; the per-service tightening and the
 *            per-user home partition are the forward work (B2.3/B2.4).  A user
 *            is separated from root by the ACLs, not yet by the mask.
 *   guest    windows only — the one level whose MASK is genuinely narrowed.
 *
 * A static inline so it costs nothing and both freestanding kernel and
 * userland translation units can use it without a linked object.
 */
static inline unsigned int caps_for_level(int level) {
  switch (level) {
  case PLVL_MACHINE:
  case PLVL_ROOT:
  case PLVL_USER:
    return CAP_ALL;
  case PLVL_GUEST:
    return CAP_WINDOW;
  default:
    return 0u;
  }
}

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

/*
 * fd redirection on spawn (Phase 4, shell `<`/`>`/`>>`/`2>`).  The PARENT opens
 * the redirect targets (getting handles in its OWN table), then hands the kernel
 * a list of {child_fd <- parent_fd} pairs; SYS_SPAWN dups each parent handle
 * into the child's fd slot, OVERWRITING the pre-installed console (fork+dup2
 * semantics, no string marshaling).  A child reading fd 0 / writing fd 1 then
 * hits the FILE object instead of the terminal.  The parent closes its own
 * copies after the spawn returns.
 */
#define SPAWN_MAX_REDIR 8
struct spawn_redir {
  int child_fd;  /* slot in the CHILD's table (0=stdin,1=stdout,2=stderr,…) */
  int parent_fd; /* an open handle to dup down, in source_pid's table */
  /* source_pid: whose table `parent_fd` belongs to.
   *   0  = the SPAWNER's own table (the ordinary case: a shell redirecting its
   *        own descriptors into its own child).
   *   >0 = ANOTHER process's table.  This exists because an execution SERVICE
   *        spawns on a client's behalf, so the fds belong to the CLIENT, not to
   *        the spawner.
   *
   * AUTHORITY: a non-zero source_pid is accepted only from a PRIVILEGED caller.
   * Reaching into another process's handle table is a system-service power, not
   * an application one — the same rule, and the same reasoning, as
   * OBJ_CTL_SETOWNER.  Without the gate any process could siphon descriptors
   * out of any other by naming them here. */
  int source_pid;
};

#endif /* NEXS_API_CAPS_H */
