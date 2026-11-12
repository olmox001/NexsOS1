/*
 * NXEXEC is the public execution front-end. The existing resolver, ELF
 * classification and redirection code is shared with the supervised service
 * layer. Hosted graphical/terminal execution now belongs to nxttyd, which
 * provides one userland VT/ANSI surface per job and transfers output via the
 * existing object/PORT transport. The caller never becomes a ctty.
 */
#include "nxexec.h"
#include <os1.h>

int main(int argc, char *argv[]) {
  if (argc < 2 || argc > NXEXEC_ARGV_MAX - 1) return 1;
  char *args[NXEXEC_ARGV_MAX];
  args[0] = (char *)"/sys/services/nxttyd";
  for (int i = 1; i < NXEXEC_ARGV_MAX; ++i)
    args[i] = i < argc ? argv[i] : NULL;
  long pid = execsvc_launch_explicit(args[0], argc, args,
                                     SPAWN_FLAG_DETACHED, NULL, 0);
  if (pid <= 0) return 1;
  /* Preserve the ORIGINAL nxexec host PID/lifetime while the terminal owns
   * a windowless job. nxfilem registers this PID as a viewer and prunes it
   * with wait(); exiting immediately made the viewer disappear from its
   * supervision list while its graphical TTY and child were still alive.
   * For GUI children, nxttyd's debounced classification exits promptly and
   * this launcher then exits too, preserving detached GUI independence.
   * No alternate spawn/resolver path or special treatment of app names. */
  int status = 0;
  for (uint64_t epoch = 0U; epoch < UINT64_MAX; ++epoch) {
    int result = OS1low_process_wait_status((int)pid, &status);
    if (result != -1) return status < 0 ? 1 : status & 0xff;
    OS1_sleep(15);
  }
  return 1;
}
