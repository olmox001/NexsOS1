/*
 * kernel/lib/registry.c
 * Dynamic System Registry (Key-Value Store)
 * "Military Grade" Implementation: Fixed memory, no dynamic allocation, strict
 * bounds.
 *
 * Purpose:
 *   Provides a global flat key-value store for kernel and userland configuration.
 *   Keys use dotted-path notation (e.g. "theme.color", "system.hostname").
 *   The store is pre-populated with three default entries at init time.
 *
 * Role:
 *   Exposed to userland via the sys_registry() syscall (op=REG_OP_READ/WRITE).
 *   Used internally by the desktop/UI layer for theme and system settings.
 *   The sched.h include is present for a planned but not yet implemented
 *   permission check on writes.
 *
 * Data Model:
 *   A static array of MAX_REGISTRY_KEYS (128) struct registry_entry, each
 *   holding a key[64], value[128], and used flag.  Total static cost: 128 ×
 *   (64 + 128 + 4) = ~24 KB of BSS.  No heap allocation is used.
 *
 * Lookup:
 *   O(n) linear scan of registry_store[] on every read and write.  At 128
 *   entries this is negligible in practice.
 *
 * Known issues:
 *   LIB-REG-01  (W3 WRONG-DESIGN)  The store is a flat 128-slot array with no
 *               tree, no enumeration, no file semantics, and no permissions.
 *               Dotted-key notation suggests hierarchy, but lookup is a flat scan.
 *               See docs/review/analysis/07-lib-headers.md §5 for the Plan 9 gap
 *               analysis.  The correct architecture is a /reg synthetic VFS node.
 *   LIB-REG-02  (W3 SECURITY)  No permission check on writes.  Any process can
 *               call sys_registry(REG_OP_WRITE, "system.hostname", ...) and
 *               overwrite system-level entries.  The sched.h include and comment
 *               "if needed later" mark this as deferred; it has not been done.
 *   LIB-REG-03  (W1 BAD-IMPL)  <kernel/vmm.h> is included twice (lines 13 and
 *               16 of the original file, both unconditional).
 *   LIB-REG-04  (W2 MISSING)   No API to enumerate keys.  Userland cannot
 *               discover registry contents without knowing key names in advance.
 */

#include <kernel/printk.h>
#include <kernel/registry.h>
#include <kernel/sched.h> /* For current_process/permissions check if needed later */
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vmm.h>
#include <stdbool.h>

/* NOTE(LIB-REG-03): <kernel/vmm.h> is included a second time here.
 * The duplicate is harmless (include guards prevent double-declaration) but
 * is a code-quality defect.  It should be removed. */
#include <kernel/vmm.h>

/* registry_store[]: the flat static key-value table.
 * Capacity: MAX_REGISTRY_KEYS (128) entries, each 196 bytes; ~24 KB total BSS.
 * NOTE(LIB-REG-01): flat array — no tree, no hierarchy, O(n) scan per op. */
static struct registry_entry registry_store[MAX_REGISTRY_KEYS];
/* registry_count: number of slots currently marked used (informational; used
 * only for the init log message; not consulted during lookup or insert). */
static int registry_count = 0;
/* registry_lock: global spinlock protecting registry_store[] and registry_count.
 * Acquired with IRQ save/restore so the store is safe to access from IRQ context.
 * NOTE(LIB-REG-02): the lock protects data integrity but not access permissions;
 * any caller (any privilege level) can write any key. */
static DEFINE_SPINLOCK(registry_lock);

/*
 * registry_init - zero the store and install default entries.
 *
 * Must be called once during early kernel init, before any other registry
 * or syscall operation.  Not thread-safe on re-entry (no guard); assumed to
 * run single-threaded before SMP is active.
 *
 * Default entries: "theme.color"="dark", "system.hostname"="NeXs",
 * "mouse.sensitivity"="1.0".
 *
 * Locking: calls registry_set() which acquires registry_lock per call.
 * Side effects: zeroes registry_store[] BSS; increments registry_count
 *               for each default entry added.
 */
void registry_init(void) {
  memset(registry_store, 0, sizeof(registry_store));
  registry_count = 0;

  /* Set default values */
  registry_set("theme.color", "dark");
  registry_set("system.hostname", "NeXs");
  registry_set("mouse.sensitivity", "1.0");

  pr_info("Registry: Initialized with %d default keys.\n", registry_count);
}

/*
 * registry_set - create or update a key-value pair.
 *
 * First scans for an existing entry with the matching key (O(n)); if found,
 * updates its value in place.  Otherwise, scans for a free slot (second O(n)
 * pass) and inserts a new entry.  Both key and value are truncated to
 * (MAX_KEY_LEN - 1) and (MAX_VAL_LEN - 1) characters respectively, and are
 * always NUL-terminated.
 *
 * NOTE(LIB-REG-01): two sequential O(n) scans; at 128 entries this is fine,
 *   but does not scale to a large store.
 * NOTE(LIB-REG-02): no permission check; any caller can write any key.
 *
 * Params:
 *   key   - NUL-terminated key string; must be non-NULL.
 *   value - NUL-terminated value string; must be non-NULL.
 * Returns: 0 on success, -1 if key or value is NULL or store is full.
 * Locking: acquires registry_lock with IRQ save/restore.
 */
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

/*
 * registry_get - look up a key and copy its value into a caller buffer.
 *
 * O(n) linear scan.  On hit, copies at most (size - 1) bytes of the value
 * and NUL-terminates buffer.
 *
 * Params:
 *   key    - NUL-terminated key to look up; must be non-NULL.
 *   buffer - output buffer of at least 'size' bytes; must be non-NULL.
 *   size   - capacity of buffer including NUL slot; must be > 0.
 * Returns: 0 on success (key found and value copied), -1 if not found or
 *          if key or buffer is NULL.
 * Locking: acquires registry_lock with IRQ save/restore.
 */
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
 * sys_registry - syscall handler for registry access (syscall 250).
 *
 * Mediates userland access to the kernel registry via secure user-space
 * copy helpers.  All pointer arguments are validated through vmm before
 * any data is consumed.
 *
 * op == REG_OP_WRITE (1):
 *   1. Copies 'key' string from user-space (up to MAX_KEY_LEN bytes).
 *   2. Copies 'value' string from user-space (up to MAX_VAL_LEN bytes).
 *   3. Calls registry_set() to create or update the entry.
 *
 * op == REG_OP_READ (0):
 *   1. Copies 'key' string from user-space.
 *   2. Calls registry_get() to fetch the value into a kernel buffer.
 *   3. Copies at most min(strlen+1, size) bytes back to user-space 'value'.
 *
 * NOTE(LIB-REG-02): No permission check on writes.  Any process at any
 *   privilege level can overwrite any key, including "system.hostname".
 *   The sched.h include is present for a future check using
 *   current_process->permissions, which has not been implemented.
 *
 * NOTE(LIB-REG-04): No enumeration op.  Userland must know keys in advance.
 *
 * Params:
 *   op    - REG_OP_READ (0) or REG_OP_WRITE (1).
 *   key   - user-space pointer to NUL-terminated key string.
 *   value - user-space pointer to value buffer (read: output; write: input).
 *   size  - capacity of user-space value buffer (bytes).
 * Returns: 0 on success; -1 on invalid pointer, key not found, or store full;
 *          -2 on unrecognised op.
 * Locking: does not hold any lock across the vmm copy calls; registry_set/get
 *          each internally acquire registry_lock.
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
