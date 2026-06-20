#ifndef _USER_NXPERM_H
#define _USER_NXPERM_H

/*
 * user/sys/bin/nxperm.h
 * NEXS users/permissions helper (ASTRA service layer) — FOUNDATION.
 *
 * The privilege model (include/api/caps.h): every process has one LEVEL —
 * machine (the computer itself, NOT a login user), root, user, guest — which is
 * the resolver for the (future) multi-user model, plus a capability MASK
 * enforced at the syscall boundary.  nxperm presents this as a LEVEL-based view
 * so applications reason in terms of machine/root/user/guest and never touch the
 * raw capability bits (ASTRA goal: hide the capability layer behind a
 * kernel-managed standard).
 *
 * This header is the INTROSPECTION foundation only.  The full vision — login,
 * named users + passwords, su elevation, UAC approval popups rendered as
 * kernel-trusted windows, and a capability-partitioned VFS — is the dedicated
 * users phase.
 *
 * SECURITY: read-only, adds no checks.  Identity comes from OS1_identity(),
 * which only ever reveals the caller's OWN level/mask — secure by caller.
 */
#include <os1.h> /* pulls caps.h (PLVL_ and CAP_ macros) + OS1_identity + printf */
#include <string.h>

/* Level name (machine/root/user/guest), or "?" out of range. */
static inline const char *nxperm_level_name(int level) {
  switch (level) {
  case PLVL_MACHINE: return "machine";
  case PLVL_ROOT:    return "root";
  case PLVL_USER:    return "user";
  case PLVL_GUEST:   return "guest";
  default:           return "?";
  }
}

/* The capability mask a level grants — the user-facing abstraction over the
 * kernel's level_ceiling[] (kernel/sched/process.c is the single source of
 * truth; mirrored here for the userland view).  machine/root/user = full,
 * guest = windows only. */
static inline unsigned int nxperm_level_mask(int level) {
  switch (level) {
  case PLVL_MACHINE:
  case PLVL_ROOT:
  case PLVL_USER:  return CAP_ALL;
  case PLVL_GUEST: return CAP_WINDOW;
  default:         return 0u;
  }
}

/* One capability bit -> a human service label. */
struct nxperm_cap {
  unsigned int bit;
  const char *label;
};
static const struct nxperm_cap NXPERM_CAPS[] = {
    {CAP_SPAWN, "spawn"},     {CAP_FS_WRITE, "files"},
    {CAP_IPC_ANY, "ipc"},     {CAP_WINDOW, "windows"},
    {CAP_REG_WRITE, "registry"},
};
#define NXPERM_NCAPS (int)(sizeof(NXPERM_CAPS) / sizeof(NXPERM_CAPS[0]))

/* Render a cap mask as a space-separated label list into buf ("-" if none). */
static inline void nxperm_mask_str(unsigned int mask, char *buf, int n) {
  int p = 0;
  if (n > 0)
    buf[0] = '\0';
  for (int i = 0; i < NXPERM_NCAPS; i++) {
    if (!(mask & NXPERM_CAPS[i].bit))
      continue;
    const char *l = NXPERM_CAPS[i].label;
    if (p && p + 1 < n)
      buf[p++] = ' ';
    for (int k = 0; l[k] && p + 1 < n; k++)
      buf[p++] = l[k];
    buf[p] = '\0';
  }
  if (p == 0 && n > 1) {
    buf[0] = '-';
    buf[1] = '\0';
  }
}

/* A declared system service + the capability needed to use it (ASTRA: every
 * service is gated by a capability — only programs granted that capability may
 * activate it).  Foundation list; extended as services land. */
struct nxperm_service {
  const char *name;
  const char *desc;
  unsigned int cap;
};
static const struct nxperm_service NXPERM_SERVICES[] = {
    {"windows", "create/draw windows", CAP_WINDOW},
    {"spawn", "launch programs", CAP_SPAWN},
    {"files", "write files", CAP_FS_WRITE},
    {"registry", "write registry", CAP_REG_WRITE},
    {"ipc", "message non-kin", CAP_IPC_ANY},
};
#define NXPERM_NSERVICES                                                        \
  (int)(sizeof(NXPERM_SERVICES) / sizeof(NXPERM_SERVICES[0]))

static inline void nxperm_emit(int win, const char *s) {
  if (win < 0)
    printf("%s", s);
  else
    printf_win(win, "%s", s);
}

/* "whoami": current level + the services it may use. */
static inline void nxperm_print_identity(int win) {
  int level = 0;
  unsigned int mask = 0;
  OS1_identity(&level, &mask);
  char m[96];
  nxperm_mask_str(mask, m, (int)sizeof(m));
  char line[160];
  snprintf(line, sizeof(line), "level: %s\ncan:   %s\n",
           nxperm_level_name(level), m);
  nxperm_emit(win, line);
}

/* List the levels and the mask each grants. */
static inline void nxperm_print_levels(int win) {
  nxperm_emit(win, "LEVEL    SERVICES\n");
  for (int lv = 0; lv < PLVL_COUNT; lv++) {
    char m[96];
    nxperm_mask_str(nxperm_level_mask(lv), m, (int)sizeof(m));
    char line[160];
    snprintf(line, sizeof(line), "%-8s %s\n", nxperm_level_name(lv), m);
    nxperm_emit(win, line);
  }
}

/* List the declared services + the capability each requires, and whether the
 * current caller holds it. */
static inline void nxperm_print_services(int win) {
  int level = 0;
  unsigned int mask = 0;
  OS1_identity(&level, &mask);
  nxperm_emit(win, "SERVICE   ALLOWED  DESCRIPTION\n");
  for (int i = 0; i < NXPERM_NSERVICES; i++) {
    const struct nxperm_service *s = &NXPERM_SERVICES[i];
    char line[160];
    snprintf(line, sizeof(line), "%-9s %-8s %s\n", s->name,
             (mask & s->cap) ? "yes" : "no", s->desc);
    nxperm_emit(win, line);
  }
}

#endif /* _USER_NXPERM_H */
