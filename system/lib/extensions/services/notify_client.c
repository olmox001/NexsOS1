/*
 * NX-SDK service extension: notification client.
 *
 * NX-VIOLA owns the notification server. The public client acquires an
 * attenuated SEND capability for the named system service PORT and posts one
 * bounded IPC_TYPE_NOTIFY. No PID is trusted or guessed. The compatibility
 * srv.notify_pid registry entry is not used for client authorization.
 *
 * Error classification is also an extension because its visible reporting
 * path depends on the boot-started notification service. The C/POSIX runtime
 * sets errno and calls this policy through the public OS1_report_error seam.
 */
#include <errno.h>
#include <limits.h>
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
  if (err == INT_MIN)
    return; /* No errno has this value; avoid signed negation overflow. */
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
    if (OS1_notify_error(ctx, strerror(err)) != 0) return;
    return;
  case EACCES:
  case EPERM:
    if (OS1_notify_warn(ctx, strerror(err)) != 0) return;
    return;
  default:
    return; /* normal control-flow error: no notification */
  }
}

static int notify_send(const char *title, const char *msg, int sev) {
  if (title == NULL || msg == NULL)
    return -EINVAL;
  struct ipc_message imsg = {0};
  imsg.type = IPC_TYPE_NOTIFY;
  imsg.data1 = (uint64_t)sev;
  imsg.data2 = 0;
  size_t i = 0U;
  /* Pack "title: msg" into the 64-byte payload field.
   * 30-char limit for title leaves room for ": " and at least 32 msg chars. */
  for (; i < 30U && *title; ++i)
    imsg.payload[i] = *title++;
  imsg.payload[i++] = ':';
  imsg.payload[i++] = ' ';
  for (; i < sizeof(imsg.payload) - 1U && *msg; ++i)
    imsg.payload[i] = *msg++;
  imsg.payload[i] = '\0';
  /* Use the normal object PORT capability, never a mutable registry PID.
   * Kernel-stamped sender and bounded queue semantics are shared with the
   * execution, time and diagnostic services. No fallback to ambient PID IPC. */
  /* This is the bottommost error-reporting transport. Do not call the
   * POSIX port convenience wrappers: their errno_ret_ctx hook invokes
   * OS1_report_error and would recursively send another notification if the
   * service is missing. The same object operations and kernel capability
   * checks remain authoritative, but there is no recursive report hook. */
  long port = OS1low_handle_create(
      OS1_NS_PORT, OS1NX_PORT_NOTIFY, OS1_RIGHT_WRITE, OBJ_TYPE_PORT);
  if (port < 0)
    return (int)port;
  /* Empty-transfer PORT sends use the same authenticated bounded queue but
   * return EAGAIN instead of sleeping behind a stalled notification broker.
   * The raw SDK veneer deliberately avoids errno/report-error recursion. */
  long sent = _sys_port_send_caps((int)port, &imsg, NULL, 0);
  long closed = OS1low_handle_close((int)port);
  if (sent != (long)sizeof(imsg))
    return sent < 0 ? (int)sent : -EIO;
  return closed == 0 ? 0 : (int)closed;
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
