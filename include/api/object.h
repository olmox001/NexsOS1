/*
 * include/api/object.h
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
/* Reserved for later migrations (ASTRA §6.2/§6.7): windows, gpu, audio.
 * #define OBJ_TYPE_WINDOW  4 */

/* Access rights — a per-handle subset, separable and attenuable (seL4). */
#define OS1_RIGHT_READ      (1u << 0) /* OS1_object_read                       */
#define OS1_RIGHT_WRITE     (1u << 1) /* OS1_object_write                      */
#define OS1_RIGHT_WAIT      (1u << 2) /* OS1_object_wait                       */
#define OS1_RIGHT_DUPLICATE (1u << 3) /* OS1low_handle_duplicate               */
#define OS1_RIGHT_TRANSFER  (1u << 4) /* OS1low_cap_grant to another process   */
#define OS1_RIGHT_DESTROY   (1u << 5) /* destroy the underlying object         */
#define OS1_RIGHT_ALL                                                          \
  (OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_WAIT | OS1_RIGHT_DUPLICATE |   \
   OS1_RIGHT_TRANSFER | OS1_RIGHT_DESTROY)

/* Namespaces for handle_create: how the `path` argument is interpreted. */
#define OS1_NS_FS   1 /* path is a filesystem path → OBJ_TYPE_FILE            */
#define OS1_NS_PROC 2 /* path is a decimal PID string → OBJ_TYPE_PROCESS      */
#define OS1_NS_REG  3 /* path is a registry key (dotted) → OBJ_TYPE_REGKEY    */

/* cap_query packs the object type and the held rights into one return value:
 * (type << 24) | rights.  A negative return is an errno (-EBADF). */
#define OS1_CAPQ_PACK(type, rights) (((long)((type) & 0xFF) << 24) | ((rights) & 0x00FFFFFF))
#define OS1_CAPQ_TYPE(v)            (((v) >> 24) & 0xFF)
#define OS1_CAPQ_RIGHTS(v)          ((v) & 0x00FFFFFF)

#endif /* NEXS_API_OBJECT_H */
