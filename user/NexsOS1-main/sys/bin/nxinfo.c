/*
 * user/sys/bin/nxinfo.c
 * NEXS system-information frontend (ASTRA stratified service model).
 *
 * Usage:
 *   nxinfo        print a one-shot system summary (OS version, uptime, live
 *                 process count, desktop resolution, pid, cwd)
 *
 * This is the THIN CLI frontend over the reusable helper nxinfo.h: it holds no
 * policy of its own and only reads state the kernel already lets this caller
 * read, so it is "secure by caller" — an unprivileged user and a system service
 * each see exactly what the kernel grants them.
 */
#include "nxinfo.h"
#include <os1.h>

int main(int argc, char **argv) {
  if (argc >= 2) {
    printf("nxinfo: unexpected argument '%s'\n", argv[1]);
    printf("usage: nxinfo\n");
    return 1;
  }
  /* -1 → emit to the caller's stdout/own window via printf. */
  nxinfo_print_summary(-1);
  return 0;
}
