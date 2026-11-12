/*
 * nxexecd: ROOT execution broker. Port ownership is the readiness contract.
 *
 * Requests use kernel-translated capability handles and a bounded out-of-line
 * body. The kernel-stamped m->from alone designates the requesting owner.
 * Request/response I/O, cwd restoration and granted-handle cleanup are
 * explicit; a failed operation never gets rewritten to a successful reply.
 */
#include <execsvc.h>
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
  struct os1_port_packet *packet;
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
    if (i >= bytes) continue;
    dest[i] = (uint8_t)ctx->body[i];
  }
  ctx->granted_count = ctx->hdr.nredir;
  *offset = bytes;
  return 0;
}

static int execsvc_next_string(struct execsvc_ctx *ctx, unsigned offset) {
  if (offset >= ctx->hdr.body_len) return -EINVAL;
  for (unsigned i = 0U; i < EXECSVC_BODY_MAX &&
                         offset + i < ctx->hdr.body_len; ++i) {
    if (ctx->body[offset + i] == '\0') return (int)(offset + i + 1U);
  }
  return -EINVAL;
}

static int execsvc_parse(struct execsvc_ctx *ctx) {
  unsigned offset = 0U;
  int rc = execsvc_decode_redirs(ctx, &offset);
  if (rc != 0) return rc;
  int next = execsvc_next_string(ctx, offset);
  if (next < 0) return next;
  ctx->cwd = ctx->body + offset;
  offset = (unsigned)next;
  for (int i = 0; i < EXECSVC_ARG_MAX && i < ctx->hdr.argc; ++i) {
    next = execsvc_next_string(ctx, offset);
    if (next < 0) return next;
    ctx->argv[i] = ctx->body + offset;
    offset = (unsigned)next;
  }
  return offset == ctx->hdr.body_len ? 0 : -EINVAL;
}

static int execsvc_make_redirs(struct execsvc_ctx *ctx) {
  for (int i = 0; i < SPAWN_MAX_REDIR; ++i) {
    if (i >= ctx->granted_count) continue;
    struct execsvc_redir *item = &ctx->granted[i];
    struct spawn_redir *dst = &ctx->redirection[i];
    if (item->source != EXECSVC_FD_GRANTED &&
        item->source != EXECSVC_FD_CLIENT_FD)
      return -EINVAL;
    if (item->ref < 0 || item->child_fd < 0) return -EBADF;
    dst->child_fd = item->child_fd;
    if (item->source == EXECSVC_FD_GRANTED) {
      if ((unsigned)item->ref >= ctx->packet->cap_count - 2U) return -EBADF;
      unsigned char *bytes = (unsigned char *)&dst->parent_fd;
      size_t offset = (2U + (unsigned)item->ref) * sizeof(int);
      for (unsigned j = 0U; j < sizeof(int); ++j)
        bytes[j] = (unsigned char)ctx->packet->message.payload[offset + j];
    } else dst->parent_fd = item->ref;
    dst->source_pid = item->source == EXECSVC_FD_GRANTED ? 0 :
                      ctx->requester;
    ++ctx->redirection_count;
  }
  return 0;
}

static int execsvc_open_owner(int requester) {
  char key[16];
  int n = snprintf(key, sizeof(key), "%d", requester);
  if (n <= 0 || (size_t)n >= sizeof(key)) return -EOVERFLOW;
  return (int)OS1low_handle_create(OS1_NS_PROC, key, OS1_RIGHT_DESTROY,
                                   OBJ_TYPE_PROCESS);
}

static int execsvc_run_spawn(struct execsvc_ctx *ctx) {
  int rc = execsvc_make_redirs(ctx);
  if (rc != 0) return rc;
  int owner = execsvc_open_owner(ctx->requester);
  if (owner < 0) return owner;
  int adopted = ctx->cwd[0] != '\0';
  if (adopted && chdir(ctx->cwd) != 0) {
    int error = -errno;
    if (OS1low_handle_close(owner) != 0)
      OS1_report_error("nxexecd.owner_close", -EIO);
    return error;
  }
  struct execsvc_owned_launch launch = {
      .owner_handle = owner, .argc = ctx->hdr.argc,
      .flags = ctx->hdr.flags, .nredir = ctx->redirection_count,
      .redirections = ctx->redirection
  };
  for (int i = 0; i < EXECSVC_ARG_MAX && i < launch.argc; ++i)
    launch.argv[i] = ctx->argv[i];
  int pid = execsvc_launch_owned(&launch, ctx->result.resolved,
                                  sizeof(ctx->result.resolved));
  int restored = adopted ? chdir("/") : 0;
  long closed = OS1low_handle_close(owner);
  /* No cleanup error can undo an acknowledged child or justify executing
   * the request again. Its owner was assigned before it became runnable. */
  if (restored != 0) OS1_report_error("nxexecd.cwd_restore", -EIO);
  if (closed != 0) OS1_report_error("nxexecd.owner_close", (int)closed);
  if (pid <= 0) return pid < 0 ? pid : -ENOENT;
  ctx->result.owner_pid = ctx->requester;
  return pid;
}

static void execsvc_handle_spawn(struct os1_port_packet *packet) {
  const struct ipc_message *message = &packet->message;
  /* Resident server is single-threaded. The large bounded request body
   * persists in this process, not in its narrow syscall stack. */
  static struct execsvc_ctx ctx;
  ctx = (struct execsvc_ctx){0};
  ctx.request = -1;
  ctx.reply = -1;
  ctx.requester = message->from;
  ctx.packet = packet;
  ctx.result.version = EXECSVC_VERSION;
  ctx.result.pid = -EINVAL;
  int rc = execsvc_decode_handles(&ctx, message);
  if (rc == 0) rc = execsvc_read_header(&ctx);
  if (rc == 0) rc = execsvc_parse(&ctx);
  if (rc == 0) rc = execsvc_run_spawn(&ctx);
  ctx.result.pid = rc > 0 ? rc : rc < 0 ? rc : -EIO;
  if (rc < 0) OS1_report_error("nxexecd.spawn_request", rc);
  if (ctx.reply >= 0) {
    int written = execsvc_write_reply(ctx.reply, &ctx.result);
    if (written != 0) OS1_report_error("nxexecd.reply_write", written);
  }
  int closed = OS1_port_packet_close_caps(packet);
  if (closed != 0) OS1_report_error("nxexecd.request_caps_close", closed);
}

static int nxexec_service(void) {
  int port = -EEXIST;
  for (unsigned attempt = 0U; attempt < EXECSVC_CLAIM_TRIES && port < 0; ++attempt) {
    port = OS1_port_create(OS1NX_PORT_EXEC);
    if (port < 0) OS1_sleep(EXECSVC_CLAIM_WAIT_MS);
  }
  if (port < 0) {
    OS1_report_error("nxexecd.port_claim", port);
    return 1;
  }
  int failed = 0;
  for (uint64_t epoch = 0U; epoch < UINT64_MAX && !failed; ++epoch) {
    struct os1_port_packet packet;
    int received = OS1_port_packet_receive(port, &packet, 0);
    if (received != 0) {
      OS1_report_error("nxexecd.port_receive", received);
      failed = 1;
    } else if (packet.message.type == EXECSVC_REQ_SPAWN &&
               packet.cap_count >= 2U) execsvc_handle_spawn(&packet);
    else {
      int released = OS1_port_packet_close_caps(&packet);
      if (released != 0) { failed = 1; OS1_report_error("nxexecd.reject_caps", released); }
    }
  }
  long closed = OS1low_handle_close(port);
  if (closed < 0) {
    OS1_report_error("nxexecd.port_close", (int)closed);
    failed = 1;
  }
  return failed;
}

int main(void) { return nxexec_service(); }
