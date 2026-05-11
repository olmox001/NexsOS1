/*
 * kernel/lib/registry.c
 * Dynamic System Registry (Key-Value Store)
 * "Military Grade" Implementation: Fixed memory, no dynamic allocation, strict
 * bounds.
 */

#include <kernel/printk.h>
#include <kernel/registry.h>
#include <kernel/sched.h> /* For current_process/permissions check if needed later */
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vmm.h>
#include <stdbool.h>

#include <kernel/vmm.h>

static struct registry_entry registry_store[MAX_REGISTRY_KEYS];
static int registry_count = 0;
static DEFINE_SPINLOCK(registry_lock);

void registry_init(void) {
  memset(registry_store, 0, sizeof(registry_store));
  registry_count = 0;

  /* Set default values */
  registry_set("theme.color", "dark");
  registry_set("system.hostname", "NeXs");
  registry_set("mouse.sensitivity", "1.0");

  pr_info("Registry: Initialized with %d default keys.\n", registry_count);
}

int registry_set(const char *key, const char *value) {
  if (!key || !value)
    return -1;

  uint64_t flags;
  spin_lock_irqsave(&registry_lock, &flags);

  /* Search for existing key to update */
  for (int i = 0; i < MAX_REGISTRY_KEYS; i++) {
    if (registry_store[i].used && strcmp(registry_store[i].key, key) == 0) {
      strncpy(registry_store[i].value, value, MAX_VAL_LEN - 1);
      registry_store[i].value[MAX_VAL_LEN - 1] = '\0';
      spin_unlock_irqrestore(&registry_lock, flags);
      return 0;
    }
  }

  /* Find free slot */
  for (int i = 0; i < MAX_REGISTRY_KEYS; i++) {
    if (!registry_store[i].used) {
      strncpy(registry_store[i].key, key, MAX_KEY_LEN - 1);
      registry_store[i].key[MAX_KEY_LEN - 1] = '\0';

      strncpy(registry_store[i].value, value, MAX_VAL_LEN - 1);
      registry_store[i].value[MAX_VAL_LEN - 1] = '\0';

      registry_store[i].used = 1;
      registry_count++;
      spin_unlock_irqrestore(&registry_lock, flags);
      return 0;
    }
  }

  pr_err("Registry: Storage full! Cannot add '%s'\n", key);
  spin_unlock_irqrestore(&registry_lock, flags);
  return -1;
}

int registry_get(const char *key, char *buffer, size_t size) {
  if (!key || !buffer)
    return -1;

  uint64_t flags;
  spin_lock_irqsave(&registry_lock, &flags);

  for (int i = 0; i < MAX_REGISTRY_KEYS; i++) {
    if (registry_store[i].used && strcmp(registry_store[i].key, key) == 0) {
      strncpy(buffer, registry_store[i].value, size - 1);
      buffer[size - 1] = '\0';
      spin_unlock_irqrestore(&registry_lock, flags);
      return 0;
    }
  }
  spin_unlock_irqrestore(&registry_lock, flags);
  return -1; /* Not found */
}

/*
 * Syscall Interface
 * op: 0 = READ, 1 = WRITE
 */
long sys_registry(int op, const char *key, char *value, size_t size) {
  char k_key[MAX_KEY_LEN];
  char k_val[MAX_VAL_LEN];

  /* 1. Copy Key from User Space securely (stops at null!) */
  if (vmm_copy_string_from_user(k_key, key, MAX_KEY_LEN) != 0) {
    pr_err("%s", "sys_registry: Invalid key pointer\n");
    return -1;
  }

  if (op == REG_OP_WRITE) {
    /* 2. Copy Value from User Space securely (stops at null!) */
    if (vmm_copy_string_from_user(k_val, value, MAX_VAL_LEN) != 0) {
      pr_err("%s", "sys_registry: Invalid value pointer\n");
      return -1;
    }
    return registry_set(k_key, k_val);
  } else if (op == REG_OP_READ) {
    if (registry_get(k_key, k_val, sizeof(k_val)) == 0) {
      /* 3. Copy Result to User Space securely */
      size_t copy_len = strlen(k_val) + 1;
      if (copy_len > size)
        copy_len = size;

      if (vmm_copy_to_user(value, k_val, copy_len) != 0) {
        pr_err("%s", "sys_registry: Failed to copy back to user\n");
        return -1;
      }
      return 0;
    }
    return -1;
  }

  return -2; /* Invalid op */
}
