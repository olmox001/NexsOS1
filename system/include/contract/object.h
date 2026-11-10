/*
 * include/abi/object.h
 * Object / handle / capability ABI — the single source of truth shared by the
 * kernel (kernel/include/kernel/object.h) and userland (os1.h), exactly like
 * caps.h.  This is the REAL capability layer mandated by ASTRA §6.1/6.2/6.5
 * (seL4 capability semantics, Mach ports-as-objects, NT per-process handle
 * table): authority is an UNFORGEABLE handle to a kernel OBJECT, not ambient
 * identity (a PID/level mask).
 *
 * A handle is a small non-negative integer naming a slot in the calling
 * process's PRIVATE handle table.  The integer is meaningless in any other
 * process and cannot be forged into authority: a value with no installed slot
 * is -EBADF.  Each handle carries a RIGHTS subset; rights are separable
 * (a handle may hold READ without WRITE) and attenuable (duplicate/grant can
 * only drop rights, never add — escalation is impossible by construction,
 * mirroring the monotonic spawn cut in caps.h).
 *
 * Relationship to the B3 model: caps.h (CAP_* level mask) is the AMBIENT
 * authority that gates *acquiring* a capability (e.g. CAP_FS_WRITE to open a
 * file for write); object.h is the per-object capability you then hold and
 * delegate.  The B3 per-process fd table (kernel/fd.h) is the seed this
 * generalizes (ASTRA §6.2).
 */
#ifndef NEXS_API_OBJECT_H
#define NEXS_API_OBJECT_H

/* Object types (kobject.type and the `type` argument of handle_create). */
#define OBJ_TYPE_NONE    0
#define OBJ_TYPE_FILE    1 /* a VFS-backed file (read/write/seek by offset)   */
#define OBJ_TYPE_PROCESS 2 /* a process: wait on exit, query, capability IPC  */
#define OBJ_TYPE_REGKEY  3 /* a registry key: read/write its value (§6.6)     */
#define OBJ_TYPE_WINDOW  4 /* a compositor window: read info / minimize / restore /
                            * focus / close via a handle (ASTRA §6.7)         */
#define OBJ_TYPE_CONSOLE 5 /* a controlling-terminal stream (stdin/stdout/stderr):
                            * read() drains keyboard input, write() goes to the
                            * caller's window/ctty.  Handles 0/1/2 are pre-installed
                            * as this (ASTRA §6.2 "Input device" + window-stream);
                            * the per-process fd table folded into it.         */
#define OBJ_TYPE_PIPE    6 /* an anonymous byte pipe: a kernel ring buffer with a
                            * READ end and a WRITE end (separate handles/rights).
                            * read() blocks until data or the last writer closes
                            * (EOF); write() appends.  Created by SYS_PIPE; the
                            * shell wires `cmd | cmd` by dup'ing the ends into the
                            * children's fd 0/1 (ASTRA §6.2).                   */
#define OBJ_TYPE_PORT    7 /* named service mailbox capability */
#define OBJ_TYPE_TIMER   8 /* read-only system time snapshot capability */
#define OBJ_TYPE_RESOURCE 9 /* provider-defined hardware/resource capability */
#define OBJ_TYPE_COUNT   10 /* number of distinct OBJ_TYPE_* values */
/* Reserved for later migrations (ASTRA §6.2/§6.7): gpu, audio. */

/* Access rights — a per-handle subset, separable and attenuable (seL4). */
#define OS1_RIGHT_READ      (1u << 0) /* OS1_object_read                       */
#define OS1_RIGHT_WRITE     (1u << 1) /* OS1_object_write                      */
#define OS1_RIGHT_WAIT      (1u << 2) /* OS1_object_wait                       */
#define OS1_RIGHT_DUPLICATE (1u << 3) /* OS1low_handle_duplicate               */
#define OS1_RIGHT_TRANSFER  (1u << 4) /* OS1low_cap_grant to another process   */
#define OS1_RIGHT_DESTROY   (1u << 5) /* destroy the underlying object         */
#define OS1_RIGHT_MUTATE    (1u << 7) /* mutate the namespace rooted at a DIR  */
#define OS1_RIGHT_ALL                                                          \
  (OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_WAIT | OS1_RIGHT_DUPLICATE |   \
   OS1_RIGHT_TRANSFER | OS1_RIGHT_DESTROY | OS1_RIGHT_MUTATE)
/* OS1_RIGHT_CREATE — ACQUISITION-ONLY flag for handle_create(OS1_NS_FS): a
 * missing path is created as an empty FILE through the provider (ASTRA §6.8,
 * open(O_CREAT) → handle_create), gated by the same vfs_write_allowed seam as
 * every write.  Meaningful only together with OS1_RIGHT_WRITE.  NOT part of
 * OS1_RIGHT_ALL: it is stripped before the handle is installed, so it never
 * appears in cap_query results and cannot be duplicated/granted. */
#define OS1_RIGHT_CREATE    (1u << 6)
/* Acquisition-only private service PORT creation. The creator must have
 * MACHINE authority, and only a delegated SEND handle reaches userland.
 * The flag is stripped from installed rights and cannot be inherited. */
#define OS1_PORT_CREATE_PRIVATE (1u << 8)

/* Control verbs for OS1_object_ctl(handle, cmd, arg) — type-specific actions
 * on an object you hold a capability to (seL4: the right IS the authority). */
#define OBJ_CTL_KILL     1 /* PROCESS: terminate the target (needs OS1_RIGHT_DESTROY)  */
#define OBJ_CTL_MINIMIZE 2 /* WINDOW: hide to background, dock-restorable (RIGHT_WRITE) */
#define OBJ_CTL_RESTORE  3 /* WINDOW: show + raise + focus (RIGHT_WRITE)                */
#define OBJ_CTL_FOCUS    4 /* WINDOW: give keyboard focus + raise (RIGHT_READ — focus is
                            * unprivileged, matching the compositor's open click-to-focus) */
#define OBJ_CTL_CLOSE    5 /* WINDOW: destroy just this window (RIGHT_DESTROY)          */
#define OBJ_CTL_SEEK     6 /* FILE: set the object's byte offset to arg; returns it     */
#define OBJ_CTL_STAT     7 /* FILE: return the current size in bytes (lseek/stat)       */
#define OBJ_CTL_STOP     8 /* PROCESS: suspend the target (job control; RIGHT_DESTROY)  */
#define OBJ_CTL_CONT     9 /* PROCESS: resume a stopped target (job control; DESTROY)   */
#define OBJ_CTL_SETOWNER 11 /* PROCESS: set the target's LOGICAL parent (owner) to
                             * `arg` (needs OS1_RIGHT_DESTROY on the target AND a
                             * PRIVILEGED caller).  Used by an execution service
                             * that spawns on a client's behalf: the spawn makes
                             * the SERVICE the mechanical parent, this hands the
                             * job back to the requesting CLIENT so kill/stop/cont
                             * authority (an ancestry walk) still reaches it.
                             * Privileged-only because it DELEGATES authority over
                             * a process — it is not a hint (ASTRA §6.5).       */
#define OBJ_CTL_TRUNCATE 10 /* FILE: truncate to `arg` bytes (needs RIGHT_WRITE).  The
                             * provider primitive is whole-file-replace (an offset-0
                             * write REPLACES the file), so only arg == 0 (truncate to
                             * empty) is expressible here — the case a POSIX
                             * write(fd,...,0) CANNOT express, since that is a no-op.
                             * Non-empty lengths are composed in libc's ftruncate()
                             * (read the head back, rewrite it from offset 0) and never
                             * reach the kernel; arg != 0 returns -EINVAL.    */
#define OBJ_CTL_MKDIR    12 /* DIR: create the one-component name at user pointer
                             * `arg` (needs RIGHT_MUTATE).  The parent directory
                             * capability names the namespace being changed.      */
/* PORT: non-blocking dequeue. arg is a user struct ipc_message*; READ right
 * required. Returns message bytes, -EAGAIN when empty, -EPIPE if closed. */
#define OBJ_CTL_PORT_TRY_RECV 14
#define OBJ_CTL_UNLINK   13 /* DIR: remove the one-component name at user pointer
                             * `arg` (needs RIGHT_MUTATE).  Recursive removal is
                             * deliberately a userland composition.              */

/* Namespaces for handle_create: how the `path` argument is interpreted. */
#define OS1_NS_FS   1 /* path is a filesystem path → OBJ_TYPE_FILE            */
#define OS1_NS_PROC 2 /* path is a decimal PID string → OBJ_TYPE_PROCESS      */
#define OS1_NS_REG  3 /* path is a registry key (dotted) → OBJ_TYPE_REGKEY    */
#define OS1_NS_WIN  4 /* path is a decimal window id → OBJ_TYPE_WINDOW        */
#define OS1_NS_PORT 5 /* path is a service port NAME → OBJ_TYPE_PORT (§6.5).
                       * Acquiring with OS1_RIGHT_READ (receive right) CREATES
                       * the port and makes the caller its owner — that is how a
                       * service publishes itself; acquiring without READ yields
                       * a SEND-only capability to an existing port, which is how
                       * a client reaches a SERVICE BY NAME instead of by pid. */
#define OS1_NS_TIME 6 /* path "system" → privileged read-only OBJ_TYPE_TIMER */
#define OS1_NS_RESOURCE 7 /* provider resource name; authority is the handle */

/* cap_query packs the object type and the held rights into one return value:
 * (type << 24) | rights.  A negative return is an errno (-EBADF). */
#define OS1_CAPQ_PACK(type, rights) (((long)((type) & 0xFF) << 24) | ((rights) & 0x00FFFFFF))
#define OS1_CAPQ_TYPE(v)            (((v) >> 24) & 0xFF)
#define OS1_CAPQ_RIGHTS(v)          ((v) & 0x00FFFFFF)

/* Window state bits for struct window_info.flags (ASTRA §6.7: windows as
 * objects).  A window manager (e.g. /sys/bin/nxui, the dock) reads these to lay
 * out its app list; an app reads its own window's bits via OS1_object_read. */
#define WININFO_VISIBLE   (1u << 0) /* currently composited (shown)             */
#define WININFO_MINIMIZED (1u << 1) /* sent to background, dock-restorable      */
#define WININFO_TOPMOST   (1u << 2) /* always-on-top overlay, no decorations    */
#define WININFO_FOCUSED   (1u << 3) /* owns keyboard focus                      */
#define WININFO_PASSIVE   (1u << 4) /* click-through (system popup)             */
#define WININFO_PROTECTED (1u << 5) /* chrome actions disabled by window policy */

/* One enumerated window.  Returned in bulk by SYS_WINDOW_ENUM / OS1_window_enum
 * and singly by OS1_object_read() on an OBJ_TYPE_WINDOW handle.  `title` mirrors
 * struct window.title[64] in the compositor. */
struct window_info {
  int id;             /* compositor window id                  */
  int pid;            /* owning process                        */
  int x, y;           /* on-screen position                    */
  int w, h;           /* on-screen draw size                   */
  unsigned int flags; /* WININFO_* bitmask                     */
  char title[64];     /* window title                          */
};

#endif /* NEXS_API_OBJECT_H */
