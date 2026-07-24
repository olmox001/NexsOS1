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
 * NX_ABI_ASSERT(cond, msg) — a compile-time claim about a layout or constant.
 *
 * There was NOT ONE static assertion in this tree before this file, and the
 * cost of that showed up concretely: `struct pt_regs` is built by hand in
 * assembly (syscall.S pushes the fields one by one) and read as a C struct by
 * the dispatcher.  Nothing tied the two together, so a field inserted in the
 * struct would have silently shifted every offset the assembly writes — and
 * the symptom would have been a kernel that writes through a pointer into its
 * own text, hundreds of instructions away from the edit.  That is precisely
 * the shape of the panic that motivated this header.
 */
#define NX_ABI_ASSERT(cond, msg) _Static_assert((cond), msg)

/* NX_ASSERT_OFFSET / NX_ASSERT_SIZE — the two forms actually needed.
 * Use them wherever assembly, another architecture, or a persisted on-disk
 * format depends on a layout: the assert is the contract, and it is checked on
 * every build of every arch instead of being remembered. */
#define NX_ASSERT_OFFSET(type, field, off)                                     \
  NX_ABI_ASSERT(__builtin_offsetof(type, field) == (off),                      \
                #type "." #field " moved: assembly and/or the other arch "    \
                      "depend on this offset")

#define NX_ASSERT_SIZE(type, bytes)                                            \
  NX_ABI_ASSERT(sizeof(type) == (bytes),                                       \
                "sizeof(" #type ") changed: check every consumer that "       \
                "hardcodes it (assembly, the other arch, on-disk images)")

#endif /* _KERNEL_NX_CONTRACT_H */
