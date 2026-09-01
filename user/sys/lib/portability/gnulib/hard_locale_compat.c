/*
 * user/sys/lib/portability/gnulib/hard_locale_compat.c
 * NexsOS1 locale compatibility layer for Gnulib hard_locale() function.
 *
 * Coreutils (via gnulib) calls hard_locale(category) to check if the
 * current locale for that category (LC_TIME, LC_COLLATE, etc.) is non-C.
 * NexsOS1 runs ONLY in the C/POSIX locale with no locale switching,
 * so we provide minimal stubs here that declare NexsOS1 is always in C locale.
 *
 * This avoids compiling gnulib's complex setlocale_null.c, which has
 * threading infrastructure and platform-specific logic we don't need.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* SETLOCALE_NULL_MAX: max size for locale category name buffer.
 * Standard gnulib definition. */
#define SETLOCALE_NULL_MAX (256+1)

/* setlocale_null_r_unlocked: get the current locale category name without locking.
 * In NexsOS1, always returns "C" for any category.
 * Returns 0 on success, EINVAL if category is invalid. */
int setlocale_null_r_unlocked(int category, char *buf, size_t bufsize) {
  /* Validate category */
  if (category < 0) {
    return 22; /* EINVAL */
  }

  /* NexsOS1 is always in C locale */
  const char *locale_name = "C";
  size_t needed = strlen(locale_name) + 1;

  if (bufsize < needed) {
    return 34; /* ERANGE */
  }

  if (buf) {
    strncpy(buf, locale_name, bufsize - 1);
    buf[bufsize - 1] = '\0';
  }
  return 0; /* Success */
}

/* setlocale_null_r: thread-safe version (in NexsOS1, no threading needed).
 * Returns error code like setlocale_null_r_unlocked. */
int setlocale_null_r(int category, char *buf, size_t bufsize) {
  /* No threading in freestanding NexsOS1, so just call the unlocked version */
  return setlocale_null_r_unlocked(category, buf, bufsize);
}

/* hard_locale: determine if the current locale category is "hard" (non-C).
 * A "hard" locale has special behavior (collation, formatting, etc.).
 * NexsOS1 always runs in C/POSIX locale, so this always returns false. */
bool hard_locale(int category) {
  (void)category; /* Unused in NexsOS1 */
  return false;
}
