#ifndef _ABI_NX_ABI_H
#define _ABI_NX_ABI_H
/*
 * nx_abi.h — compile-time layout contracts for structures that cross a
 * boundary this compiler cannot see across.
 *
 * WHY THIS IS IN abi/ AND NOT IN kernel/.  Three consumers share these
 * structures and only one of them is the kernel:
 *
 *   - the kernel, compiled for amd64 AND for aarch64;
 *   - userland, compiled separately against its own api headers;
 *   - hand-written assembly, which addresses fields by constant offset.
 *
 * Nothing in the build makes those agree.  A field added to a shared struct
 * recompiles every consumer happily and changes what each of them means by the
 * same bytes — and the symptom appears wherever the mismatch is first read,
 * never where it was introduced.  `kernel/nx_contract.h` includes THIS header
 * rather than defining its own copy, because two definitions of one contract
 * is the duplication failure these asserts exist to catch.
 *
 * Use them on: anything in an IPC message, anything a syscall copies to or
 * from userland, anything assembly touches, and anything written to disk.
 */

/* NX_ABI_ASSERT - a claim checked at compile time, on every build of every
 * arch and of userland.  C11 `_Static_assert` is available in both toolchains
 * this project uses; there is no runtime cost and no way to skip it. */
#define NX_ABI_ASSERT(cond, msg) _Static_assert((cond), msg)

/* NX_ASSERT_OFFSET - pin a field's position.  The message names the consumers
 * that break, because "offsetof changed" tells the next reader nothing about
 * what to go and look at. */
#define NX_ASSERT_OFFSET(type, field, off)                                     \
  NX_ABI_ASSERT(__builtin_offsetof(type, field) == (off),                      \
                #type "." #field " moved: assembly, the other architecture "   \
                      "and/or userland address this by constant offset")

/* NX_ASSERT_SIZE - pin a whole structure.  Size matters independently of
 * offsets: an array of these is indexed by stride, so growing the struct moves
 * every element even when no field moved. */
#define NX_ASSERT_SIZE(type, bytes)                                            \
  NX_ABI_ASSERT(sizeof(type) == (bytes),                                       \
                "sizeof(" #type ") changed: any array of it is indexed by "    \
                "stride, so every element moves — check userland, the other "  \
                "architecture, and on-disk images")

#endif /* _ABI_NX_ABI_H */
