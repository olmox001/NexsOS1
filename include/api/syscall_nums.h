/*
 * include/api/syscall_nums.h
 * NEXS syscall numbers — THE single source of truth (ABI-01/ABI-SYS-01).
 *
 * Included by BOTH sides of the ABI:
 *   - kernel/core/syscall_dispatch.c (the dispatch switch)
 *   - include/api/os1.h (userland API)
 *   - user/arch/{aarch64,amd64}/syscall.S and user/sys/lib/syscall.S
 *     (the .S stubs are preprocessed, so they use these macros directly)
 * A number changed here changes everywhere atomically; there is no second
 * table to drift out of sync.
 *
 * #define-only on purpose: this header must stay assembler-safe.
 *
 * Numbering: the POSIX-shaped calls keep their Linux-aarch64 numbers
 * (63/64/93/169/172) for familiarity; NEXS-specific calls live in the
 * 200..299 block.  The legacy duplicate IPC numbers (30/31/32) are GONE:
 * SEND/RECV/TRY_RECV are 230/231/233 only.
 *
 * Error model (ABI-02): every syscall returns a negative errno value from
 * <posix_types.h> on failure (-EFAULT, -ENOMEM, -EINVAL, ...) and >= 0 on
 * success — the Linux kernel convention.  No global errno is consumed by
 * the kernel; userland wrappers may derive one if they wish.
 */
#ifndef _SYSCALL_NUMS_H
#define _SYSCALL_NUMS_H

/* --- POSIX-shaped --- */
#define SYS_OPEN               56  /* open(path, flags) -> fd (ABI-03) */
#define SYS_CLOSE              57
#define SYS_LSEEK              62
#define SYS_READ               63
#define SYS_WRITE              64
#define SYS_EXIT               93
#define SYS_GET_TIME           169
#define SYS_GETPID             172

/* --- Graphics / compositor --- */
#define SYS_DRAW               200
#define SYS_FLUSH              201
#define SYS_CREATE_WINDOW      210
#define SYS_WINDOW_DRAW        211
#define SYS_COMPOSITOR_RENDER  212
#define SYS_WINDOW_BLIT        213
#define SYS_WINDOW_SET_FLAGS   214
#define SYS_DESTROY_WINDOW     215
#define SYS_WINDOW_WRITE       217  /* write text to a window by id (#123) */
#define SYS_WINDOW_OF_PID      218  /* window id of a pid, 0 if none (#123) */
#define SYS_WINDOW_GRID        219  /* terminal grid of a window: (cols<<16)|rows */

/* --- Memory --- */
#define SYS_SBRK               216

/* --- Processes --- */
#define SYS_SPAWN              220
#define SYS_KILL               221
#define SYS_GETPROCS           222
#define SYS_YIELD              223
#define SYS_SPAWN_CAPS         234  /* spawn_caps(path, level, caps) — USR-SEC-03 #79 */
#define SYS_WAIT               247

/* --- IPC --- */
#define SYS_SEND               230
#define SYS_RECV               231
#define SYS_SET_FOCUS          232
#define SYS_TRY_RECV           233

/* --- Registry / files / misc --- */
#define SYS_REGISTRY           250
#define SYS_FILE_WRITE         251
#define SYS_FILE_READ          252
#define SYS_SET_FONT           253
#define SYS_LIST_DIR           254
#define SYS_CHDIR              255
#define SYS_GETCWD             256
#define SYS_NANOSLEEP          257  /* real blocking sleep (POSIX nanosleep); arg0 = nanoseconds */
#define SYS_CLOCK_GETTIME      258  /* Tier 3 clock; arg0: 0=MONOTONIC ns, 1=process CPU ns; returns ns */

#endif /* _SYSCALL_NUMS_H */
