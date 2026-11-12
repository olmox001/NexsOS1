/*
 * user/sys/lib/extensions/services/execsvc_client.c
 * Client stub for the execution service (Phase 9c).
 *
 * Deliberately its OWN translation unit rather than more code inside lib.c:
 * Phase 10a/12 will split service clients out of the libc entirely, and lib.c is
 * already the file whose entanglement those phases exist to undo.  Starting it
 * separate means that split is a Makefile move, not another extraction.
 *
 * WHAT THIS IS FOR: nxexec is meant to be THE standard executor and the POSIX
 * compatibility point for execution.  system() historically bypassed it
 * completely (it spawned the shell itself), so the POSIX surface had its own
 * private execution path — exactly the divergence the daemon exists to remove.
 *
 * WHAT IT IS NOT FOR: interactive foreground jobs.  The service becomes the
 * spawning parent, and while OBJ_CTL_SETOWNER restores AUTHORITY it does NOT
 * move the controlling terminal, so Ctrl-Z/Ctrl-C would break.  That is Phase 9d
 * (ctty handback), and until then interactive jobs keep the in-process path.
 */
#include <execsvc.h>
#include <os1.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

struct execsvc_request {
  int service, request[2], reply[2];
  struct execsvc_spawn_hdr header;
  unsigned used;
  char body[EXECSVC_BODY_MAX];
};

static int execsvc_write_full(int fd, const void *data, unsigned size) {
  if (size > EXECSVC_BODY_MAX) return -EOVERFLOW;
  unsigned done = 0U;
  for (unsigned step = 0U; step < EXECSVC_BODY_MAX && done < size; ++step) {
    long n = write(fd, (const char *)data + done, size - done);
    if (n < 0) return -errno;
    if (n == 0) return -EPIPE;
    if ((unsigned long)n > size - done) return -EIO;
    done += (unsigned)n;
  }
  return done == size ? 0 : -EIO;
}

static int execsvc_read_reply(int fd, struct execsvc_spawn_rep *reply) {
  size_t done = 0U;
  for (size_t step = 0U; step < sizeof(*reply) && done < sizeof(*reply); ++step) {
    long n = read(fd, (char *)reply + done, sizeof(*reply) - done);
    if (n < 0) return -errno;
    if (n == 0) return -EPIPE;
    if ((unsigned long)n > sizeof(*reply) - done) return -EIO;
    done += (size_t)n;
  }
  if (done != sizeof(*reply) || reply->version != EXECSVC_VERSION ||
      reply->pid == 0) return -EINVAL;
  return reply->pid;
}

static int execsvc_close_fd(int *fd) {
  if (*fd < 0) return 0;
  int result = close(*fd);
  if (result != 0) return -errno;
  *fd = -1;
  return 0;
}

static int execsvc_release_request(struct execsvc_request *request) {
  int status = 0;
  for (unsigned i = 0U; i < 2U; ++i) {
    int a = execsvc_close_fd(&request->request[i]);
    int b = execsvc_close_fd(&request->reply[i]);
    if (status == 0 && a != 0) status = a;
    if (status == 0 && b != 0) status = b;
  }
  if (request->service >= 0) {
    long closed = OS1low_handle_close(request->service);
    if (status == 0 && closed != 0) status = (int)closed;
    if (closed == 0) request->service = -1;
  }
  return status;
}

static int execsvc_append_argument(struct execsvc_request *request,
                                    const char *text) {
  if (text == NULL || request->used >= sizeof(request->body)) return -EINVAL;
  size_t size = strnlen(text, EXECSVC_BODY_MAX);
  if (size >= EXECSVC_BODY_MAX ||
      size + 1U > sizeof(request->body) - request->used) return -E2BIG;
  if (memcpy(request->body + request->used, text, size + 1U) !=
      request->body + request->used) return -EIO;
  request->used += (unsigned)(size + 1U);
  return 0;
}

static int execsvc_pack_request(struct execsvc_request *request,
                                 int argc, char *const argv[], unsigned flags) {
  char cwd[EXECSVC_PATH_MAX];
  int status = getcwd(cwd, sizeof(cwd));
  if (status != 0) return status;
  status = execsvc_append_argument(request, cwd);
  if (status != 0) return status;
  for (int i = 0; i < EXECSVC_ARG_MAX && i < argc; ++i) {
    status = execsvc_append_argument(request, argv[i]);
    if (status != 0) return status;
  }
  request->header = (struct execsvc_spawn_hdr){
    .version = EXECSVC_VERSION, .flags = flags,
    .argc = argc, .body_len = request->used
  };
  return 0;
}

static int execsvc_request_channels(struct execsvc_request *request) {
  request->service = OS1_port_open(OS1NX_PORT_EXEC);
  if (request->service < 0) return -errno;
  if (pipe(request->request) != 0 || pipe(request->reply) != 0) return -errno;
  struct ipc_message message = {.type = EXECSVC_REQ_SPAWN};
  int give[2] = {request->request[0], request->reply[1]};
  long sent = OS1_port_send_caps(request->service, &message, give, 2);
  if (sent != (long)sizeof(message)) return sent < 0 ? -errno : -EIO;
  int a = execsvc_close_fd(&request->request[0]);
  int b = execsvc_close_fd(&request->reply[1]);
  return a != 0 ? a : b;
}

static int execsvc_exchange_request(struct execsvc_request *request) {
  int status = execsvc_request_channels(request);
  if (status != 0) return status;
  status = execsvc_write_full(request->request[1], &request->header,
                               sizeof(request->header));
  if (status == 0)
    status = execsvc_write_full(request->request[1], request->body, request->used);
  int closed = execsvc_close_fd(&request->request[1]);
  if (status != 0) return status;
  if (closed != 0) return closed;
  struct execsvc_spawn_rep reply;
  return execsvc_read_reply(request->reply[0], &reply);
}

int execsvc_spawn(int argc, char *const argv[], unsigned int flags) {
  if (argc <= 0 || argc > EXECSVC_ARG_MAX || argv == NULL ||
      (flags & ~SPAWN_FLAGS_ALL) != 0U) return -EINVAL;
  struct execsvc_request request = {
    .service = -1, .request = {-1, -1}, .reply = {-1, -1}
  };
  int result = execsvc_pack_request(&request, argc, argv, flags);
  if (result == 0) result = execsvc_exchange_request(&request);
  int closed = execsvc_release_request(&request);
  /* A confirmed child must never be launched twice through caller fallback
   * merely because teardown of a separate transport descriptor failed. */
  if (closed != 0) OS1_report_error("execsvc.channel_close", closed);
  return result;
}

/* The shared executable resolver is deliberately in the SDK: callers and the
 * execution broker cannot diverge in quoting, HOME, cwd or PATH semantics. */
const char *execsvc_strip_path_quotes(const char *name, char *out, size_t size) {
  if (!name || !out || size == 0U) return NULL;
  size_t n = strlen(name);
  if (n >= 2U && ((name[0] == '"' && name[n - 1U] == '"') ||
                  (name[0] == '\'' && name[n - 1U] == '\''))) {
    n -= 2U;
    if (n >= size) return NULL;
    for (size_t i = 0U; i < EXECSVC_PATH_MAX && i < n; ++i) {
      out[i] = name[i + 1U];
    }
    out[n] = '\0';
    return out;
  }
  return name;
}

/* 0=resolved pathname, 1=bare name requiring /bin then /sys/bin search,
 * negative errno=invalid/truncated input. No implicit truncation. */
int execsvc_resolve_path(const char *name, char *out, size_t size) {
  if (!name || !name[0] || !out || size < 2U) return -EINVAL;
  char clean[EXECSVC_PATH_MAX];
  const char *candidate = execsvc_strip_path_quotes(name, clean, sizeof(clean));
  if (!candidate || !candidate[0]) return -ENAMETOOLONG;
  int n;
  if (candidate[0] == '/') {
    n = snprintf(out, size, "%s", candidate);
    return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
  }
  if (candidate[0] == '~') {
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = EXECSVC_HOME_DEFAULT;
    if (candidate[1] == '\0')
      n = snprintf(out, size, "%s", home);
    else if (candidate[1] == '/')
      n = snprintf(out, size, "%s%s", home, candidate + 1);
    else
      return -EINVAL;
    return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
  }
  if (strchr(candidate, '/') != NULL) {
    char cwd[EXECSVC_PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != 0) return -EIO;
    size_t len = strlen(cwd);
    n = snprintf(out, size, len && cwd[len - 1U] == '/'
                                 ? "%s%s" : "%s/%s", cwd, candidate);
    return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
  }
  return 1;
}

int execsvc_launch_explicit(const char *path, int argc, char *const argv[],
                            unsigned int flags, const struct spawn_redir *redirs,
                            int nredir) {
  if (!path || !path[0] || argc < 0 || argc > EXECSVC_LOCAL_ARG_MAX ||
      nredir < 0 || nredir > SPAWN_MAX_REDIR || (nredir && !redirs) ||
      (flags & ~SPAWN_FLAGS_ALL) != 0U) return -EINVAL;
  return (int)OS1low_process_spawn_redir(path, argc, argv, flags, redirs, nredir);
}

int execsvc_resolve_user_program(const char *input, char *resolved,
                                size_t capacity) {
  if (input == NULL || resolved == NULL || capacity < 8U) return -EINVAL;
  char clean[EXECSVC_PATH_MAX];
  const char *name = execsvc_strip_path_quotes(input, clean, sizeof(clean));
  if (name == NULL || name[0] == '\0') return -ENAMETOOLONG;
  int choice = execsvc_resolve_path(name, resolved, capacity);
  if (choice < 0) return choice;
  if (choice != 0) {
    int n = snprintf(resolved, capacity, "/bin/%s", name);
    if (n < 0 || (size_t)n >= capacity) return -ENAMETOOLONG;
  }
  return strncmp(resolved, "/bin/", 5U) == 0 ? 0 : -EPERM;
}

int execsvc_launch_owned(const struct execsvc_owned_launch *launch,
                         char *resolved, size_t capacity) {
  if (launch == NULL || launch->owner_handle < 0 || launch->argc <= 0 ||
      launch->argc > EXECSVC_LOCAL_ARG_MAX || launch->nredir < 0 ||
      launch->nredir > SPAWN_MAX_REDIR ||
      (launch->nredir != 0 && launch->redirections == NULL) ||
      (launch->flags & ~SPAWN_FLAGS_ALL) != 0U) return -EINVAL;
  int status = execsvc_resolve_user_program(launch->argv[0], resolved, capacity);
  if (status != 0) return status;
  struct execsvc_owned_launch copy = *launch;
  copy.argv[0] = resolved;
  struct os1_spawn_request_v1 request = {
      .abi_version = OS1_SPAWN_REQUEST_VERSION, .struct_size = sizeof(request),
      .argc = (uint32_t)copy.argc, .flags = copy.flags,
      .nredir = (uint32_t)copy.nredir,
      .path = (uint64_t)(uintptr_t)resolved,
      .argv = (uint64_t)(uintptr_t)copy.argv,
      .redirections = (uint64_t)(uintptr_t)copy.redirections
  };
  return (int)OS1_object_ctl(copy.owner_handle, OBJ_CTL_SPAWN_OWNED,
                              (long)(uintptr_t)&request);
}

int execsvc_launch_search(int argc, char *argv[], char *resolved,
                          size_t capacity, unsigned int flags,
                          const struct spawn_redir *redirs, int nredir,
                          int user_only) {
  if (argc <= 0 || argc > EXECSVC_LOCAL_ARG_MAX || !argv || !argv[0] ||
      !resolved || capacity < 8U) return -EINVAL;
  if (user_only) {
    int status = execsvc_resolve_user_program(argv[0], resolved, capacity);
    if (status != 0) return status;
    argv[0] = resolved;
    return execsvc_launch_explicit(resolved, argc, argv, flags, redirs, nredir);
  }
  char clean[EXECSVC_PATH_MAX];
  const char *name = execsvc_strip_path_quotes(argv[0], clean, sizeof(clean));
  if (!name || !name[0]) return -ENAMETOOLONG;
  int choice = execsvc_resolve_path(name, resolved, capacity);
  if (choice < 0) return choice;
  if (choice == 0) {
    argv[0] = resolved;
    return execsvc_launch_explicit(resolved, argc, argv, flags, redirs, nredir);
  }
  int n = snprintf(resolved, capacity, "/bin/%s", name);
  if (n < 0 || (size_t)n >= capacity) return -ENAMETOOLONG;
  argv[0] = resolved;
  int pid = execsvc_launch_explicit(resolved, argc, argv, flags, redirs, nredir);
  if (pid > 0) return pid;
  n = snprintf(resolved, capacity, "/sys/bin/%s", name);
  if (n < 0 || (size_t)n >= capacity) return -ENAMETOOLONG;
  argv[0] = resolved;
  return execsvc_launch_explicit(resolved, argc, argv, flags, redirs, nredir);
}


/* A hosted application has one independent TTY supervisor and never inherits
 * the launcher's graphics/ctty. The executor (nxexec) and nxttyd retain
 * ownership of graphical classification and foreground job lifecycle. */
int execsvc_launch_hosted(int argc, char *const argv[]) {
  if (argc < 1 || argc >= EXECSVC_LOCAL_ARG_MAX || !argv || !argv[0])
    return -EINVAL;
  char *full[EXECSVC_LOCAL_ARG_MAX];
  full[0] = (char *)"/sys/bin/nxexec";
  for (int i = 0; i < EXECSVC_LOCAL_ARG_MAX - 1 && i < argc; ++i) {
    if (!argv[i]) return -EINVAL;
    full[i + 1] = argv[i];
  }
  return execsvc_launch_explicit("/sys/bin/nxexec", argc + 1, full,
                                SPAWN_FLAG_DETACHED, NULL, 0);
}

int execsvc_launch(int argc, char *argv[], enum execsvc_launch_mode mode,
                   const struct spawn_redir *redirs, int nredir,
                   char *resolved, size_t capacity) {
  if (mode == EXECSVC_MODE_HOSTED) {
    if (nredir != 0) return -EINVAL;
    if (resolved && capacity > 0U) {
      int n = snprintf(resolved, capacity, "%s", "/sys/bin/nxexec");
      if (n < 0 || (size_t)n >= capacity) return -ENAMETOOLONG;
    }
    return execsvc_launch_hosted(argc, argv);
  }
  if (mode != EXECSVC_MODE_FOREGROUND && mode != EXECSVC_MODE_DETACHED)
    return -EINVAL;
  return execsvc_launch_search(argc, argv, resolved, capacity,
      mode == EXECSVC_MODE_DETACHED ? SPAWN_FLAG_DETACHED : 0U,
      redirs, nredir, 0);
}
