# 2026-07-12 — /home + tree ACL; capability/object/registry review

Increment record (documentation is append-only). Maintainer directives:
add the user-writable /home with the three-tier tree policy, and review the
capability, object-abstraction and registry-abstraction layers.

## /home and the tree ACL (implemented)

`vfs_write_allowed` — still the single write-authority seam (S-ALIGN F6) —
now implements the maintainer's tree policy on top of the CAP_FS_WRITE
gate:

| Tree        | Write authority                              |
|-------------|----------------------------------------------|
| `/sys/bin`  | MACHINE only (even root cannot swap the supervised boot chain) |
| `/bin`, `/sys` (incl. `/sys/lib`) | ROOT or machine              |
| `/home`     | any CAP_FS_WRITE holder (the user tree)      |
| elsewhere   | any CAP_FS_WRITE holder (unchanged)          |

Exact-match guards cover the directory nodes themselves, not only children
(`unlink("/bin")` is caught, not just `/bin/x`). `/home` is created in the
rootfs build. Validated: builds green, headless boots 8/8 on both
architectures; doom (the previous fopen regression's canary) confirmed
working by the maintainer before this change.

## Review findings

**Capability layer** (`include/api/caps.h`, `sched.h` helpers, dispatch):
four-level model (machine/root/user/guest), machine bypasses cap bits, 18
enforcement points in syscall dispatch. Sound. One observation worth a
future increment: the default cap mask hands CAP_FS_WRITE to every level,
so the tree ACL — not the capability bit — is the effective write boundary
for user apps; once /home becomes the norm, consider trimming
CAP_FS_WRITE from the GUEST (and possibly USER) preset so the bit regains
meaning.

**Object abstraction** (`kernel/core/object.c`, 923 lines): the documented
invariants are actually implemented — unforgeability (private per-process
table, -EBADF on empty slots), attenuation-only (`rights & src->rights` in
grant AND duplicate, TRANSFER right + IPC authority required to delegate),
ambient-gated acquisition (write-FILE handles pass vfs_write_allowed),
pin/refcount around blocking I/O so close cannot free mid-operation, and a
single alloc/free pair keeping per-type live counts symmetric.
OBJ-GRANT-REAP (grant racing a same-instant reap on another CPU) is a
documented, accepted limitation. Follow-up queued: a line-level pass of
process_handles_destroy against handle_close for the teardown half.

**Registry abstraction** (`kernel/lib/registry.c`, 717 lines):
first-writer-wins ownership per leaf (owner 0 = system), CAP_REG_WRITE
gate, hierarchical nodes with empty-directory cleanup, mounted at /reg as
a Plan 9 namespace via vfs_mount_at. The known residual USR-SEC-01 (srv.*
key re-publish bounds a hijack to one supervisor tick) stays tracked; its
real fix is the planned OBJ_TYPE_PORT capability addressing (DIR-01 M4.5),
which this review confirms as the right target — consistent with the
maintainer's "9P over POSIX" direction: the namespace already resolves
paths to TYPED objects (vfs_resolve_object), and file APIs should keep
converging on walk/open/read/write over mounted providers.
