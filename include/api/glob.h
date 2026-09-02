#ifndef _GLOB_H
#define _GLOB_H

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal glob.h shim for NexsOS1/Gnulib compatibility. */
#define GLOB_APPEND      0x0001
#define GLOB_DOOFFS     0x0002
#define GLOB_ERR        0x0004
#define GLOB_MARK       0x0008
#define GLOB_NOCHECK    0x0010
#define GLOB_NOSORT     0x0020
#define GLOB_NOESCAPE   0x0040
#define GLOB_PERIOD     0x0080
#define GLOB_TILDE      0x0100
#define GLOB_BRACE      0x0200
#define GLOB_NOMAGIC    0x0400
#define GLOB_ONLYDIR    0x0800
#define GLOB_STAR       0x1000

#define GLOB_ABORTED    3
#define GLOB_NOMATCH    1
#define GLOB_NOSPACE    2
#define GLOB_NOSYS      (-1)

typedef struct {
  size_t gl_pathc;
  char **gl_pathv;
  size_t gl_offs;
} glob_t;

int glob(const char *pattern, int flags,
         int (*errfunc)(const char *epath, int eerrno),
         glob_t *pglob);
void globfree(glob_t *pglob);

#ifdef __cplusplus
}
#endif

#endif /* _GLOB_H */
