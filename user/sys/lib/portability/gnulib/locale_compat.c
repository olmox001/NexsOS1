/*
 * user/sys/lib/portability/gnulib/locale_compat.c
 * NexsOS1 locale compatibility stubs for Gnulib.
 *
 * Gnulib expects various locale-related functions that are not relevant
 * for NexsOS1, which runs only in C/POSIX locale. These stubs provide
 * minimal implementations that avoid linker failures.
 */

#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>  /* For timezone_t definition */

/* locale_charset: get the current character encoding.
 * NexsOS1 always uses UTF-8 for console/terminal output, but internally
 * we are C locale. Return "UTF-8". */
const char *locale_charset(void) {
  return "UTF-8";
}

/* gl_locale_name_unsafe: get the current locale name for a category.
 * Returns the locale name as a string. NexsOS1 is always C/POSIX. */
const char *gl_locale_name_unsafe(int category, const char *categoryname) {
  (void)category;
  (void)categoryname;
  return "C";
}

/* set_tz and revert_tz: timezone manipulation for strftime.
 * NexsOS1 has no timezone support; these are no-ops.
 * time_rz.c expects these to be extern and callable. */

timezone_t set_tz(timezone_t tz) {
  /* No-op: NexsOS1 has no timezone support */
  return tz;
}

bool revert_tz(timezone_t tz) {
  (void)tz;
  /* No-op: NexsOS1 has no timezone support */
  return true;
}
