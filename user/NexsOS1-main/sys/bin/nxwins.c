/*
 * user/sys/bin/nxwins.c
 * NEXS window-listing tool (ASTRA §6.7) — the textual twin of the dock
 * /sys/bin/nxui, split out of the shell into its own system program (like
 * nxproc/nxres/nxinfo).
 *
 * Usage:
 *   nxwins              list compositor windows (id/pid/state/title)
 *   nxwins focus <id>   give a window keyboard focus (reveal if backgrounded)
 *
 * Thin CLI over the same SYS_WINDOW_ENUM (OS1_window_enum) and OBJ_TYPE_WINDOW
 * focus capability (OS1_window_focus) the kernel already gates per caller — no
 * policy of its own, so nxwins's reach is exactly the caller's reach.
 */
#include <os1.h>

/*
 * cmd_list - `nxwins` (no args): enumerate and print the compositor's windows.
 */
static int cmd_list(void) {
  struct window_info wi[32];
  int n = (int)OS1_window_enum(wi, 32);
  if (n < 0) {
    printf("nxwins: error enumerating windows\n");
    return 1;
  }
  printf("\033[1;33mID   PID  STATE   TITLE\033[0m\n");
  for (int i = 0; i < n; i++) {
    const char *st = (wi[i].flags & WININFO_MINIMIZED) ? "min"
                     : (wi[i].flags & WININFO_FOCUSED)  ? "focus"
                     : (wi[i].flags & WININFO_VISIBLE)  ? "shown"
                                                        : "hidden";
    /* Phase 3: prefer nxexec's canonical launch identity (sys.proc.<pid>.name)
     * over the app's own window title, so nxwins/nxbar/dock all show one
     * stable name; fall back to the title when there is no registered id. */
    char idkey[48], idname[40];
    const char *label = wi[i].title;
    snprintf(idkey, sizeof(idkey), "sys.proc.%d.name", wi[i].pid);
    if (OS1_registry_get(idkey, idname, sizeof(idname)) == 0 && idname[0])
      label = idname;
    printf("%d  %d  %s  %s\n", wi[i].id, wi[i].pid, st, label);
  }
  return 0;
}

/*
 * cmd_focus - `nxwins focus <id>`: give a window keyboard focus.  The kernel
 * decides whether this caller may focus the target; we only relay the outcome.
 */
static int cmd_focus(const char *id_arg) {
  int id = atoi(id_arg);
  if (id <= 0) {
    printf("nxwins: invalid window id '%s'\n", id_arg);
    return 1;
  }
  int r = OS1_window_focus(id);
  if (r == 0) {
    printf("nxwins: focused window %d\n", id);
    return 0;
  }
  printf("nxwins: focus failed (%d)\n", r);
  return 1;
}

int main(int argc, char **argv) {
  /* nxwins focus <id> */
  if (argc >= 3 && strncmp(argv[1], "focus", 6) == 0)
    return cmd_focus(argv[2]);

  /* nxwins (no args): list windows. */
  if (argc < 2)
    return cmd_list();

  printf("nxwins: bad arguments\n");
  printf("usage: nxwins | nxwins focus <id>\n");
  return 1;
}
