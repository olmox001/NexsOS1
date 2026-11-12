/* NexsOS1 compatibility shim for GNU parse_datetime() support.
   This lives in the portability layer, not in the upstream gnulib source tree. */

#include <config.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "parse-datetime.h"
#include "posixtm.h"
#include "timespec.h"

static bool
parse_relative_timespec(struct timespec *result, const char *p,
                        struct timespec const *now)
{
  if (!result || !p)
    return false;

  while (*p && isspace((unsigned char) *p))
    p++;

  if (!*p)
    return false;

  if (strcmp(p, "now") == 0 || strcmp(p, "today") == 0 || strcmp(p, "this") == 0)
    {
      *result = now ? *now : current_timespec();
      return true;
    }

  char *end = NULL;
  errno = 0;
  long value = strtol(p, &end, 10);
  if (end == p || errno != 0)
    return false;

  while (*end && isspace((unsigned char) *end))
    end++;

  if (!*end)
    {
      if (now)
        {
          result->tv_sec = now->tv_sec + value;
          result->tv_nsec = now->tv_nsec;
          return true;
        }
      return false;
    }

  long seconds = 0;
  if (strncmp(end, "second", 6) == 0 || strncmp(end, "seconds", 7) == 0 ||
      strncmp(end, "sec", 3) == 0 || strncmp(end, "s", 1) == 0)
    seconds = value;
  else if (strncmp(end, "minute", 6) == 0 || strncmp(end, "minutes", 7) == 0 ||
           strncmp(end, "min", 3) == 0 || strncmp(end, "m", 1) == 0)
    seconds = value * 60;
  else if (strncmp(end, "hour", 4) == 0 || strncmp(end, "hours", 5) == 0 ||
           strncmp(end, "hr", 2) == 0 || strncmp(end, "h", 1) == 0)
    seconds = value * 3600;
  else if (strncmp(end, "day", 3) == 0 || strncmp(end, "days", 4) == 0 ||
           strncmp(end, "d", 1) == 0)
    seconds = value * 86400;
  else if (strncmp(end, "week", 4) == 0 || strncmp(end, "weeks", 5) == 0 ||
           strncmp(end, "w", 1) == 0)
    seconds = value * 604800;
  else if (strncmp(end, "month", 5) == 0 || strncmp(end, "months", 6) == 0)
    seconds = value * 30 * 86400;
  else if (strncmp(end, "year", 4) == 0 || strncmp(end, "years", 5) == 0 ||
           strncmp(end, "y", 1) == 0)
    seconds = value * 365 * 86400;
  else
    return false;

  if (now)
    {
      result->tv_sec = now->tv_sec + seconds;
      result->tv_nsec = now->tv_nsec;
      return true;
    }

  return false;
}

bool parse_datetime(struct timespec *restrict result,
                    char const *p,
                    struct timespec const *now)
{
  if (!result || !p)
    return false;

  while (*p && isspace((unsigned char) *p))
    p++;

  if (!*p)
    return false;

  if (parse_relative_timespec(result, p, now))
    return true;

  time_t secs = 0;
  if (posixtime(&secs, p, PDS_TRAILING_YEAR | PDS_CENTURY | PDS_SECONDS))
    {
      result->tv_sec = secs;
      result->tv_nsec = 0;
      return true;
    }

  return false;
}

bool parse_datetime2(struct timespec *restrict result,
                    char const *p,
                    struct timespec const *now,
                    unsigned int flags,
                    timezone_t tz,
                    char const *tzstring)
{
  (void)flags;
  (void)tz;
  (void)tzstring;
  return parse_datetime(result, p, now);
}
