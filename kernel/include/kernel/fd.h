/*
 * kernel/include/kernel/fd.h
 * Per-process file-descriptor table (ABI-03, epic #93).
 *
 * Replaces the historical overloaded-integer scheme (0=stdin/IPC,
 * 1/2=window-by-pid, >=100=window id) with a real table indexed by small
 * integers.  Three descriptor kinds exist today:
 *
 *   FD_KBD   the keyboard/stdin stream: read() drains IPC_TYPE_INPUT
 *            messages, blocking when none are pending.
 *   FD_WIN   a compositor window text sink: write() appends to the window.
 *            win_id == -1 means "the caller's own window", resolved by PID
 *            at write time (a process may create its window after spawn).
 *   FD_FILE  a VFS-backed file with a private offset; read()/write() move
 *            the offset, lseek() repositions it.  The vfs_node is a plain
 *            value (no refcount — mounts are never torn down, see vfs.h),
 *            so closing or process death needs no kernel cleanup beyond
 *            clearing the entry.  The resolved path is kept because the
 *            VFS write contract is path-based.
 *
 * fds[0]/[1]/[2] are pre-opened as KBD/WIN(-1)/WIN(-1) by process_create()
 * so existing read(0)/write(1)/write(2) callers keep working unchanged.
 * The legacy "fd >= 100 is a window id" write path remains as a
 * compatibility alias until the window ABI moves onto the table.
 *
 * Locking: none.  Processes are single-threaded and only the owning
 * process touches its table, from syscall context.  Nothing else may
 * reach into another process's fd table.
 */
#ifndef _KERNEL_FD_H
#define _KERNEL_FD_H

#include <kernel/types.h>
#include <kernel/vfs.h>

#define NPROC_FDS 16
#define FD_PATH_MAX 128

/* fd_entry.type */
#define FD_NONE 0
#define FD_KBD 1
#define FD_WIN 2
#define FD_FILE 3

/* fd_entry.mode (FD_FILE access mode, from open() O_ACCMODE) */
#define FD_MODE_READ (1 << 0)
#define FD_MODE_WRITE (1 << 1)

struct fd_entry {
  uint8_t type; /* FD_* */
  uint8_t mode; /* FD_MODE_* (FD_FILE only) */
  int win_id;   /* FD_WIN: window id, or -1 for the caller's own window */
  struct vfs_node node;   /* FD_FILE: node from vfs_open (size refreshed
                           * after writes that may grow the file) */
  uint64_t offset;        /* FD_FILE: current file position */
  char path[FD_PATH_MAX]; /* FD_FILE: resolved absolute path */
};

struct process;
/* process_fd_init - reset the table and pre-open fds 0/1/2 (KBD/WIN/WIN).
 * Called by process_create(). */
void process_fd_init(struct process *proc);

#endif /* _KERNEL_FD_H */
