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
};

void registry_init(void);
int registry_set(const char *key, const char *value);
int registry_get(const char *key, char *buffer, size_t size);

/* Syscall Handler */
long sys_registry(int op, const char *key, char *value, size_t size);

#endif
