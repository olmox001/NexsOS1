#ifndef NX_SDK_INTERNAL_H
#define NX_SDK_INTERNAL_H

#include <errno.h>
#include <os1.h>

/* nx_assert - Power-of-Ten R5 runtime assertion, compiled in only when
 * NX_STRICT is defined. VERIFIED against the actual Makefile (not assumed):
 * NX_STRICT is opt-in (`make NX_STRICT=1`), gating warn_unused_result via
 * NX_MUST_USE in kernel/nx_contract.h for the SAME stated reason this file
 * documents below — nx_contract.h's own history is that a gate you have to
 * remember to switch on caught nothing, so NX_MUST_USE was made unconditional
 * there. nx_assert() stays opt-in rather than copying that fix, because unlike
 * a compiler attribute it has a real runtime cost and an abort side effect;
 * `make NX_STRICT=1` is this project's existing "how much does the strict
 * profile catch" report (see the Makefile's own NX_STRICT comment) and this
 * hooks into that same report instead of adding a second strictness knob.
 * When active, on failure it reports through the ONE error seam
 * (OS1_report_error, prototyped in <os1.h> and already included above) with
 * EFAULT, so a violated invariant is visible the same way a hard I/O fault
 * is, then aborts via OS1low_process_exit (likewise from <os1.h>) — a
 * violated precondition here means a bug in the caller, not a recoverable
 * I/O condition, so returning an error code is not an option: continuing
 * would touch dangling handles or a NULL stream with a "confirmed working"
 * codepath around it. */
#ifdef NX_STRICT
#define nx_assert(cond)                                                      \
  do {                                                                       \
    if (!(cond)) {                                                           \
      OS1_report_error("nx_assert:" __FILE__ ":" #cond, EFAULT);             \
      OS1low_process_exit(134); /* 128+SIGABRT, matching POSIX abort() */    \
    }                                                                        \
  } while (0)
#else
/* R5/off-path: NX_STRICT off must not just vanish to ((void)0) — that leaves
 * every variable that existed only to feed a condition (e.g. `idlen` in
 * OS1low_process_wait_status) looking "assigned but never read" to
 * -Wunused-variable, and this tree builds with -Werror. `(void)(cond)`
 * still counts as a use for the compiler, still costs nothing observable
 * (the comparison's result is discarded, and `cond` must stay
 * side-effect-free by the same rule any assert() condition follows), and
 * keeps release builds warning-clean without adding an abort path. */
#define nx_assert(cond) ((void)(cond))
#endif


#endif
