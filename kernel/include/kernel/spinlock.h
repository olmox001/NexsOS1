/*
 * kernel/include/kernel/spinlock.h
 * Simple spinlock implementation for AArch64
 */
#ifndef _KERNEL_SPINLOCK_H
#define _KERNEL_SPINLOCK_H

#include <kernel/arch.h>
#include <kernel/types.h>

typedef struct {
  volatile uint32_t lock;
} spinlock_t;

#define SPINLOCK_INIT {0}
#define DEFINE_SPINLOCK(x) spinlock_t x = SPINLOCK_INIT

static inline void spin_lock_init(spinlock_t *lock) { lock->lock = 0; }

static inline void spin_lock(spinlock_t *lock) { arch_spin_lock(&lock->lock); }

static inline void spin_unlock(spinlock_t *lock) {
  arch_spin_unlock(&lock->lock);
}

static inline int spin_trylock(spinlock_t *lock) {
  return arch_spin_trylock(&lock->lock);
}

/* IRQ-safe spinlock */
static inline void spin_lock_irqsave(spinlock_t *lock, uint64_t *flags) {
  arch_local_irq_save(flags);
  /* The AArch64 internal implementation of local_irq_save already sets
     DAIF appropriately (e.g. #3 or similar for masking).
  */
  spin_lock(lock);
}

static inline int spin_trylock_irqsave(spinlock_t *lock, uint64_t *flags) {
  arch_local_irq_save(flags);
  if (spin_trylock(lock)) {
    return 1;
  }
  arch_local_irq_restore(*flags);
  return 0;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
  spin_unlock(lock);
  arch_local_irq_restore(flags);
}

#endif /* _KERNEL_SPINLOCK_H */
