/*
 * kernel/include/kernel/io_poll.h
 * Bounded busy-wait contract for driver bring-up and register polling.
 *
 * Hardware bring-up must NEVER spin forever. An absent or wedged device leaves a
 * status bit stuck — a missing 8042 floats its status port to 0xFF, a dead UART
 * never drains its TX FIFO — so an unbounded `while (!ready) ;` hangs the whole
 * boot (this is exactly what wedged the PS/2 path on BIOS-less amd64, #124).
 *
 * These macros cap the spin so the caller can degrade gracefully (skip the
 * device, drop the byte) instead of blocking. They are plain do/while macros
 * (no GNU statement-expression) so they build under -Wpedantic. `cond` is
 * re-evaluated every iteration — pass a live register read.
 */
#ifndef _KERNEL_IO_POLL_H
#define _KERNEL_IO_POLL_H

/* Default spin budget: large enough for any real register transition, small
 * enough to return promptly when the hardware is absent/wedged. */
#define POLL_SPINS_DEFAULT 1000000L

/*
 * spin_until(cond, max_spins) - bounded best-effort spin.
 *
 * Spins until `cond` becomes true or the budget runs out; the outcome is
 * discarded. Use where the caller proceeds regardless (e.g. polled console TX:
 * a wedged UART must not hang printk — drop the byte instead).
 */
#define spin_until(cond, max_spins)                                            \
  do {                                                                         \
    long _su_n = (max_spins);                                                  \
    while (!(cond) && _su_n > 0)                                               \
      _su_n--;                                                                 \
  } while (0)

/*
 * poll_until(ok, cond, max_spins) - bounded spin with outcome.
 *
 * Sets `ok` (int) to 1 if `cond` became true within the budget, 0 on timeout,
 * so the caller can skip a missing/wedged device.
 */
#define poll_until(ok, cond, max_spins)                                        \
  do {                                                                         \
    long _pu_n = (max_spins);                                                  \
    while (!(cond) && _pu_n > 0)                                               \
      _pu_n--;                                                                 \
    (ok) = (_pu_n > 0);                                                        \
  } while (0)

#endif /* _KERNEL_IO_POLL_H */
