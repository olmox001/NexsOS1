/*
 * NX-SDK service extension: notification client.
 *
 * NX-VIOLA owns and supervises nxntfy_srv. nxinit publishes its current PID
 * under srv.notify_pid after spawning the service. This client resolves that
 * endpoint for each request and sends IPC_TYPE_NOTIFY through the native IPC
 * API. An unavailable or stale endpoint returns an error; no PID is guessed.
 *
 * Error classification is also an extension because its visible reporting
 * path depends on the boot-started notification service. The C/POSIX runtime
 * sets errno and calls this policy through the public OS1_report_error seam.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <os1.h>

/*
 * OS1_report_error - THE single userland error-surfacing seam
 * (PLAN-2026-07-17-STRATIFICATION.md, Phase 0).
 *
 * Maps an errno to the notification system BY SEVERITY CLASS, so a hard
 * failure in ANY libc / portability path (SDL, lua, doom, ...) becomes visible
 * — a red/amber notification with context — instead of a silent -1 an app can
 * ignore and loop on.  Every layer calls THIS one policy; none hand-rolls its
 * own notify (no duplication, uniform behaviour).  Normal control-flow errors
 * stay silent so probes (ENOENT, EAGAIN) never spam:
 *
 *   EIO/EFAULT/ENOMEM/ENOSPC/EROFS -> error (red)   — unexpected / hard fault
 *   EACCES/EPERM                   -> warn  (amber) — policy denial
 *   everything else                -> silent        — normal control flow
 *
 * `err` may be given as errno (positive) or as a raw -errno syscall return.
 * `ctx` is a short tag: a subsystem ("vfs", "mmap") or "op path".
 */
void OS1_report_error(const char *ctx, int err) {
  if (err < 0)
    err = -err;
  if (!ctx)
    ctx = "libc";
  switch (err) {
  case EIO:
  case EFAULT:
  case ENOMEM:
  case ENOSPC:
  case EROFS:
    OS1_notify_error(ctx, strerror(err));
    break;
  case EACCES:
  case EPERM:
    OS1_notify_warn(ctx, strerror(err));
    break;
  default:
    break; /* normal control-flow error: no notification */
  }
}

static int notify_send(const char *title, const char *msg, int sev) {
  struct ipc_message imsg;
  imsg.type = IPC_TYPE_NOTIFY;
  imsg.data1 = (uint64_t)sev;
  imsg.data2 = 0;
  int i = 0;
  /* Pack "title: msg" into the 64-byte payload field.
   * 30-char limit for title leaves room for ": " and at least 32 msg chars. */
  while (*title && i < 30)
    imsg.payload[i++] = *title++;
  imsg.payload[i++] = ':';
  imsg.payload[i++] = ' ';
  while (*msg && i < 63)
    imsg.payload[i++] = *msg++;
  imsg.payload[i] = '\0';
  /* Resolve the notify_srv endpoint from the registry. If the key is absent
   * (notify_srv not up / not yet registered) FAIL instead of sending to a
   * guessed PID — the old pid=2 fallback delivered a stray IPC message to
   * whatever process happened to hold PID 2 and the notification was lost
   * anyway. Callers that need the boot popup wait for the key (see init.c). */
  char pid_buf[16];
  if (OS1_registry_get("srv.notify_pid", pid_buf, sizeof(pid_buf)) != 0)
    return -1;
  int pid = atoi(pid_buf);
  if (pid <= 0)
    return -1;
  return (int)OS1low_ipc_send(pid, &imsg);
}
int OS1_notify_post(const char *title, const char *msg) {
  return notify_send(title, msg, 0);
}
int OS1_notify_warn(const char *title, const char *msg) {
  return notify_send(title, msg, 1);
} /* yellow */
int OS1_notify_error(const char *title, const char *msg) {
  return notify_send(title, msg, 2);
} /* red — previously only the kernel crash handler (fault.c) could reach
   * severity 2 (m.data1 = 2, kernel_ipc_send bypassing this function
   * entirely); userland callers can now post one directly too. */
int notify(const char *title, const char *msg) {
  return notify_send(title, msg, 0);
} /* compat shim (DIR-01 F4) */
