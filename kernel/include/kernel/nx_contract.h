#ifndef _KERNEL_NX_CONTRACT_H
#define _KERNEL_NX_CONTRACT_H
/*
 * nx_contract.h — the vocabulary for stating, IN THE CODE, the invariants that
 * C cannot check by itself.
 *
 * WHY.  Every defect this kernel has lost days to belonged to a class the
 * language is blind to: a return value nobody looked at, a struct whose layout
 * assembly depends on, a lock held across an allocation, a page freed while a
 * register spilled onto it was still live.  Each was written into a comment
 * after it was found — and each was then violated again, because a comment
 * cannot fail a build.
 *
 * These macros exist to move an invariant from a comment into something that
 * DOES fail a build.  Two mechanisms, used for different things:
 *
 *   - COMPILER-ENFORCED (this file): warn_unused_result, nonnull, and
 *     _Static_assert.  Zero cost, no tooling, catches the error at the exact
 *     line.  Prefer these whenever the invariant is expressible.
 *   - CHECKER-ENFORCED (scripts/check-contracts.py, `make lockcheck`): the
 *     interprocedural rules — allocation under a lock, a free of memory the
 *     running context still needs, a timer outliving its object.  These need a
 *     call graph, which the C compiler will not give us.
 *
 * The annotations below are read by BOTH: the compiler acts on the attribute,
 * the checker reads the same marker to seed its analysis.  One statement, two
 * enforcers — the alternative is a second list that drifts from the first,
 * which is the duplication failure this project already paid for twice.
 */

/*
 * NX_MUST_USE — the result is not advisory.
 *
 * This is `#[must_use]`.  It exists because of EXT4-ERRNO-01: a function
 * returning -errno whose caller ignores it turns a real failure into silent
 * success, and the caller that ignores it is invisible in review.  Put it on
 * EVERY function whose return value carries a failure the caller must act on —
 * which in this tree is essentially everything returning `int` as -errno.
 */
/* Always on.  It was briefly gated behind NX_STRICT so the backlog could be
 * measured without breaking the tree, and the predictable result was that
 * nobody saw a single error: a gate you have to remember to switch on is a gate
 * that is off.  The diagnosis has to happen during an ordinary build, which is
 * the only build anyone runs. */
#define NX_MUST_USE __attribute__((warn_unused_result))

/*
 * NX_NONNULL(...) — these argument positions may never be NULL.
 *
 * 1-based indices, e.g. NX_NONNULL(1, 3).  The compiler both diagnoses a
 * literal NULL at the call site AND is allowed to assume non-NULL inside, so
 * do NOT use it on a function that deliberately tolerates NULL — pop_message()
 * is the counter-example: NULL is a reachable argument there and the guard is
 * load-bearing, not defensive.
 */
#define NX_NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))

/*
 * NX_DISCARD(expr, why) — deliberately drop a must-use result, with a reason.
 *
 * Without an escape hatch a must-use gate does not get satisfied, it gets
 * DELETED: the first person who meets a legitimately ignorable result and has
 * no way to say so removes the attribute, and the annotation is gone for every
 * caller.  So the hatch exists — and it is deliberately more typing than
 * handling the error, because the common case should be the easy one.
 *
 * `why` is a string literal that generates nothing.  Its only job is to make
 * the author state the justification where the next reader will look, so a
 * discard can be reviewed instead of merely noticed.  Note that a plain (void)
 * cast does NOT suppress warn_unused_result under GCC — the tree already had
 * `(void)arch_copy_to_user(...)` sitting in SYS_WAIT, which looked deliberate
 * and silenced nothing.
 *
 * Legitimate uses are narrow: an error-unwind path where the failure of the
 * cleanup changes nothing, and a genuinely best-effort refresh whose fallback
 * is documented.  "I do not know what to do with this" is not one of them.
 */
#define NX_DISCARD(expr, why)                                                  \
  do {                                                                         \
    __typeof__(expr) _nx_discarded __attribute__((unused)) = (expr);           \
    (void)sizeof(why);                                                         \
  } while (0)

/*
 * NX_NO_ALLOC / NX_ALLOCATES — the memory contract, for the checker.
 *
 * NX_NO_ALLOC declares "nothing on any path from here reaches the allocator",
 * which is what makes a function safe to call while a sched_lock is held.
 * NX_ALLOCATES is the opposite claim, stated so a caller cannot be surprised.
 *
 * They carry no code generation on purpose: their whole job is to be a claim
 * the checker can VERIFY against the real call graph, so a wrong annotation is
 * itself a build failure rather than a comforting lie.
 */
#define NX_NO_ALLOC
#define NX_ALLOCATES

/*
 * NX_HOLDS(lock) / NX_ACQUIRES(lock) — the lock a function expects or takes.
 * Documentation the checker can read, and the seed for the lock-order rules.
 */
#define NX_HOLDS(lock)
#define NX_ACQUIRES(lock)

/*
 * The layout-assert macros live in <abi/nx_abi.h>, NOT here.
 *
 * They are needed by userland and by the shared ABI headers too, and a second
 * definition in the kernel tree would be exactly the duplication these asserts
 * exist to catch: two statements of one contract, drifting.  See that header
 * for why the boundary is where it is.
 */
#include <nx_abi.h>

#endif /* _KERNEL_NX_CONTRACT_H */
