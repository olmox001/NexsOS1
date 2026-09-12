/*
 * user/sys/lib/portability/tinycc/tinycc_config_nexsos.h
 * NexsOS1 target configuration for TinyCC.
 *
 * Force-included (-include) on the compilation of user/bin/tinycc/tinycc.c,
 * BEFORE it #includes "tinycc.h" (mirrors coreutils_config_nexsos.h /
 * gnulib_config_nexsos.h's role for those ports).
 *
 * See README.md in this directory for the reasoning: TinyCC on NexsOS1 is
 * an AOT-only compiler (no -run / in-memory JIT execution), statically
 * linked always (no ld.so on this system), entered the same way every other
 * NexsOS1 userland binary is.
 */

#ifndef _TINYCC_CONFIG_NEXSOS_H
#define _TINYCC_CONFIG_NEXSOS_H

/* Consumed by patches/tinycc.h.patch to force TCC_IS_NATIVE off — that in
 * turn keeps the whole dlopen/dlsym/mprotect/sigaction/backtrace block in
 * tccrun.c (none of which exists/works the POSIX way on NexsOS1: mmap() is
 * heap-backed, not real page protection — see include/api/sys/mman.h — and
 * there is no dlfcn.h at all) from ever being compiled in. */
#define TCC_TARGET_NEXSOS 1

#if defined(__x86_64__)
#define TCC_TARGET_X86_64 1
#elif defined(__aarch64__)
#define TCC_TARGET_ARM64 1
#else
#error "tinycc_config_nexsos.h: unsupported NexsOS1 target architecture"
#endif

/* No <dlfcn.h> on NexsOS1 (include/api/ has no such header). tinycc.h only
 * includes it when !CONFIG_TCC_STATIC; keep this defined even though
 * TCC_IS_NATIVE ends up off too, so the port survives someone re-enabling
 * -run later without immediately breaking on a missing header again. */
#ifndef CONFIG_TCC_STATIC
#define CONFIG_TCC_STATIC 1
#endif

/* alloca(): declared in include/api/stddef.h but there is no runtime
 * definition anywhere in NexsOS1's lib.c (nothing to grow the caller's own
 * frame from a callee in this ABI). tinycc.h/tinycc.c themselves never call
 * alloca() — this exists so *programs later compiled BY tcc* that #include
 * <tcclib.h>/<stddef.h> and use alloca() get a working one instead of a
 * dangling extern. */
#ifndef alloca
#define alloca __builtin_alloca
#endif

/* ---------------------------------------------------------------------
 * Paths. All of these are #ifndef-guarded in tinycc.h itself, so defining
 * them here (force-included first) wins over the upstream defaults.
 *
 * Populated at build time by overlay.mk and installed to the rootfs at
 * exactly this path — see this directory's README.md step 4.
 *   /sys/lib/tcc/            crt1.o  crti.o  crtn.o  libc.a
 *   /sys/lib/tcc/include/    the tcc-bundled headers (tcclib.h, stdarg.h, …)
 * --------------------------------------------------------------------- */
#define CONFIG_TCCDIR "/sys/lib/tcc"
#define CONFIG_SYSROOT ""
#define CONFIG_TCC_CRTPREFIX "/sys/lib/tcc"
#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include"
#define CONFIG_TCC_LIBPATHS "{B}"

#endif /* _TINYCC_CONFIG_NEXSOS_H */