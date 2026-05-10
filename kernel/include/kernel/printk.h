/*
 * kernel/include/kernel/printk.h
 * Kernel logging interface
 */
#ifndef _KERNEL_PRINTK_H
#define _KERNEL_PRINTK_H

#include <kernel/types.h>
#include <stdarg.h>

/* Log levels */
#define KERN_EMERG 0   /* System is unusable */
#define KERN_ALERT 1   /* Action must be taken immediately */
#define KERN_CRIT 2    /* Critical conditions */
#define KERN_ERR 3     /* Error conditions */
#define KERN_WARNING 4 /* Warning conditions */
#define KERN_NOTICE 5  /* Normal but significant */
#define KERN_INFO 6    /* Informational */
#define KERN_DEBUG 7   /* Debug-level messages */

/* Current log level (messages at this level or lower are printed) */
extern int console_loglevel;

/* Core printing functions */
int printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintk(const char *fmt, va_list args);
int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

/* Convenience macros */
#define pr_emerg(fmt, ...)                                                     \
  do {                                                                         \
    if (console_loglevel >= KERN_EMERG)                                        \
      printk("[EMERG] " fmt, ##__VA_ARGS__);                                   \
  } while (0)
#define pr_alert(fmt, ...)                                                     \
  do {                                                                         \
    if (console_loglevel >= KERN_ALERT)                                        \
      printk("[ALERT] " fmt, ##__VA_ARGS__);                                   \
  } while (0)
#define pr_crit(fmt, ...)                                                      \
  do {                                                                         \
    if (console_loglevel >= KERN_CRIT)                                         \
      printk("[CRIT] " fmt, ##__VA_ARGS__);                                    \
  } while (0)
#define pr_err(fmt, ...)                                                       \
  do {                                                                         \
    if (console_loglevel >= KERN_ERR)                                          \
      printk("[ERROR] " fmt, ##__VA_ARGS__);                                   \
  } while (0)
#define pr_warn(fmt, ...)                                                      \
  do {                                                                         \
    if (console_loglevel >= KERN_WARNING)                                      \
      printk("[WARN] " fmt, ##__VA_ARGS__);                                    \
  } while (0)
#define pr_notice(fmt, ...)                                                    \
  do {                                                                         \
    if (console_loglevel >= KERN_NOTICE)                                       \
      printk("[NOTICE] " fmt, ##__VA_ARGS__);                                  \
  } while (0)
#define pr_info(fmt, ...)                                                      \
  do {                                                                         \
    if (console_loglevel >= KERN_INFO)                                         \
      printk("[INFO] " fmt, ##__VA_ARGS__);                                    \
  } while (0)
#define pr_debug(fmt, ...)                                                     \
  do {                                                                         \
    if (console_loglevel >= KERN_DEBUG)                                        \
      printk("[DEBUG] " fmt, ##__VA_ARGS__);                                   \
  } while (0)

/* Panic - halt the system */
void panic(const char *fmt, ...) __noreturn
    __attribute__((format(printf, 1, 2)));

#endif /* _KERNEL_PRINTK_H */
