#ifndef _KERNEL_REGISTRY_H
#define _KERNEL_REGISTRY_H

#include <kernel/types.h>

#define MAX_KEY_LEN 64
#define MAX_VAL_LEN 128

/* Registry Operations */
#define REG_OP_READ 0
#define REG_OP_WRITE 1
#define REG_OP_ENUM 2 /* enumerate keys into 'value' buffer (LIB-REG-04) */
#define REG_OP_DEL 3  /* remove a key (frees the node + prunes empty parents) */

void registry_init(void);
/* registry_set: create or update a key.  'owner_pid' identifies the caller:
 * 0 for kernel/system (full rights), a PID for user processes.  Updating an
 * existing key owned by someone else returns -EACCES (first-writer-wins —
 * a service's routing key cannot be hijacked by another process). */
int registry_set(const char *key, const char *value, int owner_pid);
int registry_get(const char *key, char *buffer, size_t size);
/* registry_key_is_virtual - does the key name the COMPUTED per-process view
 * (sys.proc.<pid>.*) rather than a stored node?  The authority rule belongs to
 * the key, not to the entry point: a virtual key is exempt from CAP_REG_WRITE
 * (proc_env_set applies self-or-privileged instead), so every write gate —
 * sys_registry, the OS1_NS_REG acquisition, regfs_write — consults this and
 * they cannot disagree about the same key (R2). */
int registry_key_is_virtual(const char *key);
/* registry_enum: write the newline-separated list of used keys into 'buf'
 * (NUL-terminated, bounded by 'size'); returns the number of bytes written
 * (excluding the NUL), or -1 on bad args (LIB-REG-04).
 * 'prefix' filters to keys that begin with it — the "list a namespace directory"
 * primitive (Phase 4.1 A1a); NULL or "" lists ALL keys (backward-compatible). */
int registry_enum(const char *prefix, char *buf, size_t size);
/* registry_del: remove 'key', free its node, and prune now-empty parent dirs.
 * First-writer-wins (owner_pid 0 = kernel/system).  0, -ENOENT, or -EACCES. */
int registry_del(const char *key, int owner_pid);

/* Single authority seam (S-ALIGN F5) shared by ALL THREE registry entry
 * points — sys_registry (syscall 250), the /reg VFS mount (regfs), and the
 * object manager's OBJ_TYPE_REGKEY acquisition/write path.  Gate decisions and
 * caller-identity derivation live only here so the entry points cannot drift. */
/* registry_write_allowed: mutating the registry needs CAP_REG_WRITE; a NULL
 * current_process (in-kernel caller) passes (kernel = machine identity). */
bool registry_write_allowed(void);
/* registry_caller_owner: first-writer-wins identity — 0 for machine/kernel
 * callers (full rights), the caller's PID otherwise. */
int registry_caller_owner(void);

/* Syscall Handler */
long sys_registry(int op, const char *key, char *value, size_t size);

/* registry_mount_vfs: mount the registry as the "/reg" file namespace (regfs).
 * Call from the composition root after vfs_init() + registry_init(). */
void registry_mount_vfs(void);

#endif
