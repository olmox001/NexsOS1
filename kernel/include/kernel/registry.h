#ifndef _KERNEL_REGISTRY_H
#define _KERNEL_REGISTRY_H

#include <kernel/types.h>

#define MAX_REGISTRY_KEYS 128
#define MAX_KEY_LEN 64
#define MAX_VAL_LEN 128

/* Registry Operations */
#define REG_OP_READ 0
#define REG_OP_WRITE 1

struct registry_entry {
  char key[MAX_KEY_LEN];
  char value[MAX_VAL_LEN];
  int used;
  /* owner_pid: PID that created the key (LIB-REG-02/USR-SEC-01).
   * 0 = kernel/system owner.  Only the owner (or a kernel/system caller,
   * owner_pid argument 0) may overwrite an existing key. */
  int owner_pid;
};

void registry_init(void);
/* registry_set: create or update a key.  'owner_pid' identifies the caller:
 * 0 for kernel/system (full rights), a PID for user processes.  Updating an
 * existing key owned by someone else returns -EACCES (first-writer-wins —
 * a service's routing key cannot be hijacked by another process). */
int registry_set(const char *key, const char *value, int owner_pid);
int registry_get(const char *key, char *buffer, size_t size);

/* Syscall Handler */
long sys_registry(int op, const char *key, char *value, size_t size);

#endif
