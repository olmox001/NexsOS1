/*
 * nxexecd: ROOT execution broker. Port ownership is the readiness contract.
 *
 * Requests use kernel-translated capability handles and a bounded out-of-line
 * body. The kernel-stamped m->from alone designates the requesting owner.
 * Request/response I/O, cwd restoration and granted-handle cleanup are
 * explicit; a failed operation never gets rewritten to a successful reply.
 */
#include <execsvc.h>
#include "../bin/nxexec.h"
#include <os1.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#define EXECSVC_CLAIM_TRIES 20U
#define EXECSVC_CLAIM_WAIT_MS 100U

struct execsvc_ctx {
  int request, reply, requester;
  struct execsvc_spawn_hdr hdr;
  char body[EXECSVC_BODY_MAX];
  struct execsvc_redir granted[SPAWN_MAX_REDIR];
  struct spawn_redir redirection[SPAWN_MAX_REDIR];
  char *argv[EXECSVC_ARG_MAX];
  const char *cwd;
  int granted_count, redirection_count;
  struct execsvc_spawn_rep result;
};

/* Stream I/O may be short; each iteration consumes at least one byte, and
 * the transport's maximum record size provides a static progress bound. */
static int execsvc_read_all(int fd, void *dst, unsigned size) {
  unsigned done = 0U;
  if (size > EXECSVC_BODY_MAX) return -EOVERFLOW;
  for (unsigned step = 0U; step < EXECSVC_BODY_MAX; ++step) {
    if (done == size) return 0;
    long n = read(fd, (char *)dst + done, size - done);
    if (n < 0) return -errno;
    if (n == 0) return -EPIPE;
    if ((unsigned long)n > size - done) return -EIO;
    done += (unsigned)n;
  }
  return done == size ? 0 : -EOVERFLOW;
}

static int execsvc_write_reply(int fd, const struct execsvc_spawn_rep *rep) {
  size_t done = 0U;
  const size_t size = sizeof(*rep);
  for (size_t step = 0U; step < sizeof(*rep); ++step) {
    if (done == size) return 0;
    long n = write(fd, (const char *)rep + done, size - done);
    if (n < 0) return -errno;
    if (n == 0 || (size_t)n > size - done) return -EIO;
    done += (size_t)n;
  }
  return done == size ? 0 : -EIO;
}

static int execsvc_decode_handles(struct execsvc_ctx *ctx,
                                  const struct ipc_message *message) {
  if (!ctx || !message) return -EINVAL;
  uint8_t *request = (uint8_t *)&ctx->request;
  uint8_t *reply = (uint8_t *)&ctx->reply;
  for (size_t i = 0U; i < sizeof(int); ++i) {
    request[i] = (uint8_t)message->payload[i];
    reply[i] = (uint8_t)message->payload[sizeof(int) + i];
  }
  return ctx->request >= 0 && ctx->reply >= 0 ? 0 : -EBADF;
}

static int execsvc_read_header(struct execsvc_ctx *ctx) {
  int rc = execsvc_read_all(ctx->request, &ctx->hdr, sizeof(ctx->hdr));
  if (rc != 0) return rc;
  if (ctx->hdr.version != EXECSVC_VERSION ||
      ctx->hdr.argc <= 0 || ctx->hdr.argc > EXECSVC_ARG_MAX ||
      ctx->hdr.nredir < 0 || ctx->hdr.nredir > SPAWN_MAX_REDIR ||
      ctx->hdr.body_len > EXECSVC_BODY_MAX)
    return -EINVAL;
  unsigned bytes = (unsigned)ctx->hdr.nredir *
                   (unsigned)sizeof(struct execsvc_redir);
  if (bytes > ctx->hdr.body_len) return -EINVAL;
  return execsvc_read_all(ctx->request, ctx->body, ctx->hdr.body_len);
}

/* Unaligned wire redirections are copied bytewise; their wire count is
 * checked before accessing any element of the fixed-size destination. */
static int execsvc_decode_redirs(struct execsvc_ctx *ctx, unsigned *offset) {
  unsigned bytes = (unsigned)ctx->hdr.nredir *
                   (unsigned)sizeof(struct execsvc_redir);
  uint8_t *dest = (uint8_t *)ctx->granted;
  for (unsigned i = 0U; i < SPAWN_MAX_REDIR *
       sizeof(struct execsvc_redir); ++i) {
    if (i >= bytes) break;
    dest[i] = (uint8_t)ctx->body[i];
  }
  ctx->granted_count = ctx->hdr.nredir;
  *offset = bytes;
  return 0;
}

static int execsvc_next_string(struct execsvc_ctx *ctx, unsigned *offset,
                               const char **start) {
  if (*offset >= ctx->hdr.body_len) return -EINVAL;
  *start = ctx->body + *offset;
  for (unsigned i = 0U; i < EXECSVC_BODY_MAX; ++i) {
    if (*offset >= ctx->hdr.body_len) return -EINVAL;
    if (ctx->body[(*offset)++] == '\0') return 0;
  }
  return -EOVERFLOW;
}

static int execsvc_parse(struct execsvc_ctx *ctx) {
  unsigned offset = 0U;
  int rc = execsvc_decode_redirs(ctx, &offset);
  if (rc != 0) return rc;
  rc = execsvc_next_string(ctx, &offset, &ctx->cwd);
  if (rc != 0) return rc;
  for (int i = 0; i < EXECSVC_ARG_MAX; ++i) {
    if (i >= ctx->hdr.argc) break;
    const char *arg = NULL;
    rc = execsvc_next_string(ctx, &offset, &arg);
    if (rc != 0) return rc;
    ctx->argv[i] = (char *)arg;
  }
  return offset == ctx->hdr.body_len ? 0 : -EINVAL;
}

static int execsvc_make_redirs(struct execsvc_ctx *ctx) {
  for (int i = 0; i < SPAWN_MAX_REDIR; ++i) {
    if (i >= ctx->granted_count) break;
    struct execsvc_redir *item = &ctx->granted[i];
    struct spawn_redir *dst = &ctx->redirection[i];
    if (item->source != EXECSVC_FD_GRANTED &&
        item->source != EXECSVC_FD_CLIENT_FD)
      return -EINVAL;
    if (item->ref < 0 || item->child_fd < 0) return -EBADF;
    dst->child_fd = item->child_fd;
    dst->parent_fd = item->ref;
    dst->source_pid = item->source == EXECSVC_FD_GRANTED ? 0 :
                      ctx->requester;
    ++ctx->redirection_count;
  }
  return 0;
}

static int execsvc_assign_owner(int pid, int requester) {
  char key[16];
  int n = snprintf(key, sizeof(key), "%d", pid);
  if (n <= 0 || (size_t)n >= sizeof(key)) return -EOVERFLOW;
  long handle = OS1low_handle_create(OS1_NS_PROC, key, OS1_RIGHT_DESTROY,
                                   OBJ_TYPE_PROCESS);
  if (handle < 0) return (int)handle;
  long assigned = OS1_object_ctl((int)handle, OBJ_CTL_SETOWNER, requester);
  long closed = OS1low_handle_close((int)handle);
  if (assigned != 0) return assigned < 0 ? (int)assigned : -EIO;
  if (closed != 0) return closed < 0 ? (int)closed : -EIO;
  return 0;
}

static int execsvc_spawn(struct execsvc_ctx *ctx) {
  int rc = execsvc_make_redirs(ctx);
  if (rc != 0) return rc;
  int adopted = 0;
  if (ctx->cwd[0]) {
    if (chdir(ctx->cwd) != 0) return -errno;
    adopted = 1;
  }
  char path[NXEXEC_PATH_MAX];
  int pid = nxexec_spawn_search_redir_scoped(ctx->hdr.argc, ctx->argv,
      path, (ctx->hdr.flags & SPAWN_FLAG_DETACHED) != 0,
      ctx->redirection, ctx->redirection_count, 1);
  if (adopted && chdir("/") != 0) return -EIO;
  if (pid <= 0) return pid < 0 ? pid : -ENOENT;
  rc = execsvc_assign_owner(pid, ctx->requester);
  if (rc != 0) {
    int killed = kill_process(pid);
    if (killed < 0) OS1_report_error("nxexecd.orphan_rollback", killed);
    return rc;
  }
  int n = snprintf(ctx->result.resolved, sizeof(ctx->result.resolved),
                   "%s", path);
  if (n <= 0 || (size_t)n >= sizeof(ctx->result.resolved)) {
    int killed = kill_process(pid);
    if (killed < 0) OS1_report_error("nxexecd.path_rollback", killed);
    return -EOVERFLOW;
  }
  ctx->result.owner_pid = ctx->requester;
  return pid;
}

static void execsvc_close_one(int handle, const char *phase) {
  if (handle < 0) return;
  if (close(handle) != 0) OS1_report_error(phase, errno);
}

static void execsvc_close_granted(struct execsvc_ctx *ctx) {
  execsvc_close_one(ctx->request, "nxexecd.request_close");
  execsvc_close_one(ctx->reply, "nxexecd.reply_close");
  for (int i = 0; i < SPAWN_MAX_REDIR; ++i) {
    if (i >= ctx->granted_count) break;
    if (ctx->granted[i].source == EXECSVC_FD_GRANTED)
      execsvc_close_one(ctx->granted[i].ref, "nxexecd.granted_close");
  }
}

static void execsvc_handle_spawn(const struct ipc_message *message) {
  /* Resident server is single-threaded. The large bounded request body
   * persists in this process, not in its narrow syscall stack. */
  static struct execsvc_ctx ctx;
  ctx = (struct execsvc_ctx){0};
  ctx.request = -1;
  ctx.reply = -1;
  ctx.requester = message->from;
  ctx.result.version = EXECSVC_VERSION;
  ctx.result.pid = -EINVAL;
  int rc = execsvc_decode_handles(&ctx, message);
  if (rc == 0) rc = execsvc_read_header(&ctx);
  if (rc == 0) rc = execsvc_parse(&ctx);
  if (rc == 0) rc = execsvc_spawn(&ctx);
  ctx.result.pid = rc > 0 ? rc : rc < 0 ? rc : -EIO;
  if (rc < 0) OS1_report_error("nxexecd.spawn_request", rc);
  if (ctx.reply >= 0) {
    int written = execsvc_write_reply(ctx.reply, &ctx.result);
    if (written != 0) OS1_report_error("nxexecd.reply_write", written);
  }
  execsvc_close_granted(&ctx);
}

static int nxexec_service(void) {
  int port = -EEXIST;
  for (unsigned attempt = 0U; attempt < EXECSVC_CLAIM_TRIES; ++attempt) {
    port = OS1_port_create(OS1NX_PORT_EXEC);
    if (port >= 0) break;
    OS1_sleep(EXECSVC_CLAIM_WAIT_MS);
  }
  if (port < 0) {
    OS1_report_error("nxexecd.port_claim", port);
    return 1;
  }
  int failed = 0;
  for (uint64_t epoch = 0U; epoch < UINT64_MAX; ++epoch) {
    struct ipc_message message;
    long n = OS1_port_recv(port, &message);
    if (n == 0) break;
    if (n < 0) {
      OS1_report_error("nxexecd.port_receive", (int)n);
      OS1_sleep(20);
      continue;
    }
    if (n != (long)sizeof(message)) {
      OS1_report_error("nxexecd.protocol_frame", -EIO);
      failed = 1;
      break;
    }
    if (message.type == EXECSVC_REQ_SPAWN)
      execsvc_handle_spawn(&message);
  }
  long closed = OS1low_handle_close(port);
  if (closed < 0) {
    OS1_report_error("nxexecd.port_close", (int)closed);
    failed = 1;
  }
  return failed;
}

int main(void) { return nxexec_service(); }
