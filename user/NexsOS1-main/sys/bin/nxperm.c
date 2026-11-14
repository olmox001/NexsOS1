/*
 * user/sys/bin/nxperm.c
 * NEXS users/permissions manager (ASTRA) — FOUNDATION (introspection).
 *
 * Usage:
 *   nxperm            show your identity (level + allowed services) = whoami
 *   nxperm whoami     same
 *   nxperm levels     list the privilege levels and what each grants
 *   nxperm services   list the declared services + the capability each needs
 *   nxperm su         (not yet) elevate — the system runs in debug mode, where
 *                     /sys/bin services (including the shell) already run at root
 *
 * Thin CLI over the header-only helper nxperm.h.  This is the introspection
 * foundation of the user/permission manager; login, named users + passwords, su
 * elevation and UAC approval popups (rendered as kernel-trusted windows) are the
 * dedicated users phase.  Security is "by caller": nxperm only reveals the
 * caller's own identity (OS1_identity) and never widens authority.
 */
#include "nxperm.h"
#include <os1.h>

int main(int argc, char **argv) {
  if (argc < 2 || strncmp(argv[1], "whoami", 7) == 0) {
    nxperm_print_identity(-1);
    return 0;
  }
  if (strncmp(argv[1], "levels", 7) == 0) {
    nxperm_print_levels(-1);
    return 0;
  }
  if (strncmp(argv[1], "services", 9) == 0) {
    nxperm_print_services(-1);
    return 0;
  }
  if (strncmp(argv[1], "su", 3) == 0) {
    printf("nxperm: su not yet implemented.\n");
    printf("        debug mode: /sys/bin services (incl. the shell) run at "
           "root.\n");
    printf("        login / named users / su / UAC are the dedicated users "
           "phase.\n");
    return 0;
  }
  printf("nxperm: unknown command '%s'\n", argv[1]);
  printf("usage: nxperm [whoami|levels|services|su]\n");
  return 1;
}
