/*
 * user/sys/bin/nxtop.c
 * Realtime Process List Utility - ASTRA stratified service edition.
 *
 * A thin windowed front-end over the reusable process helper nxproc.h.  It
 * opens its window and refreshes every 3 s, but the previous "spinbomb" behaviour
 * (rewriting the FULL window every tick even when nothing changed) is gone:
 * nxproc_render_if_changed() computes a signature of the visible fields and
 * skips the window write entirely when the table is unchanged.  Between checks
 * nxtop blocks on the real kernel timer (OS1_sleep) rather than busy-spinning,
 * so an idle process list costs no CPU.
 *
 * Naming: renamed from `top` to `nxtop` to follow the NEXS service naming
 * convention (every /sys/bin ELF is prefixed `nx*`).  The on-disk ELF is
 * `nxtop.elf`; the shell has no dedicated `top` builtin — typing `nxtop`
 * reaches it through the generic unknown-command fallback (spawn_search_args'
 * /bin → /sys/bin probe).  main() takes no argv: there are no options.
 */
#include "nxproc.h"
#include <os1.h>

int main(void) {
  /* Window geometry/title unchanged from the original top. */
  int my_win =
      _sys_create_window(100, 100, 520, 600, "NEXS Realtime Process List");
  if (my_win < 0)
    return 1;

  unsigned long sig = 0; /* Seed sentinel: guarantees the first render. */

  while (1) {
    /* Snapshot + signature compare + (conditional) render all live in the
     * helper; this only rewrites the window when something visible changed. */
    nxproc_render_if_changed(my_win, &sig);

    /* REFRESH RATE (~0.33 Hz): block 3 s on the real kernel timer instead
     * of busy-spinning yield(), so nxtop no longer burns a core idling. */
    OS1_sleep(3000);
  }

  return 0;
}