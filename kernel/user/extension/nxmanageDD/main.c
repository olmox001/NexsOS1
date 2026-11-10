/*
 * nxmanageDD — first NX-CORE user extension, executed in user mode.
 *
 * Only the bootstrapped PID1 receives machine authority. No process paths,
 * startup order, service names or restart policy live in Core's main().
 * This generic manager reads its initializer from a protected VFS file,
 * creates ROOT service children, and owns their real process lifecycles.
 * nxinit supplies the declarative catalog over a kernel-stamped PORT queue.
 */
#include <nxmanage.h>
#include <os1.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

struct nxm_slot {
  char path[56];
  char args[48];
  char registry[40];
  int pid;
  uint32_t mode;
  uint32_t generation;
  uint32_t backoff_ms;
  uint64_t started_ms;
  uint64_t due_ms;
  int active;
  int once_finished;
  int queued;
  int last_result;  /* real PID for successful completed one-shot, else errno */
  int ack_target;   /* pending one-shot completion reply, kernel-stamped PID */
  uint64_t deadline_ms;
};

/* Owned exclusively by the single-threaded PID1 manager event loop.
 * State cannot be accessed or reentered by IPC handlers on other CPUs. */
struct nxm_state {
  struct nxm_slot slots[NXMANAGE_SLOTS];
  char init_path[96];
  int init_pid;
  uint64_t init_due_ms;
  uint32_t init_backoff_ms;
};

static int nxm_string(const char *src, unsigned cap, unsigned *len) {
  if (src == NULL || len == NULL)
    return -1;
  for (unsigned i = 0U; i < 64U; ++i) {
    if (i >= cap)
      return -1;
    if (src[i] == '\0') {
      *len = i;
      return 0;
    }
  }
  return -1;
}

static int nxm_boot_init_path(char init_path[96]) {
  char line[102U];
  int n = file_read(NXMANAGE_INIT_CONFIG, line, sizeof(line) - 1U, 0);
  if (n < 12 || n >= (int)sizeof(line))
    return -1;
  line[n] = '\0';
  if (n > 0 && line[n-1] == '\n')
    line[--n] = '\0';
  if (n > 0 && line[n-1] == '\r')
    line[--n] = '\0';
  if (strncmp(line, "init=/sbin/", 11U) != 0 || n <= 11)
    return -1;
  for (int i = 11; i < 102; ++i) {
    if (i >= n)
      break;
    char ch = line[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
      return -1;
  }
  if (n - 4 > 96)
    return -1;
  for (int k = 0; k < 96; ++k) {
    if (k > n - 5)
      break;
    init_path[k] = line[k + 5];
  }
  return 0;
}

static int nxm_service_path(const char *path) {
  const char *rest = NULL;
  if (strncmp(path, "/sys/bin/", 9U) == 0)
    rest = path + 9;
  else if (strncmp(path, "/sys/services/", 14U) == 0)
    rest = path + 14;
  if (rest == NULL || *rest == '\0')
    return 0;
  for (unsigned i = 0U; i < 56U; ++i) {
    char ch = rest[i];
    if (ch == '\0')
      return 1;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
      return 0;
  }
  return 0;
}

static int nxm_unpack(const struct ipc_message *message,
                      struct nxm_slot *candidate) {
  const char *base = message->payload;
  unsigned offset = 0U;
  char *dest[3] = {candidate->path, candidate->args, candidate->registry};
  unsigned capacities[3] = {sizeof(candidate->path), sizeof(candidate->args),
                             sizeof(candidate->registry)};
  for (unsigned field = 0U; field < 3U; ++field) {
    unsigned length = 0U;
    if (offset >= sizeof(message->payload) ||
        nxm_string(base + offset,
                   (unsigned)sizeof(message->payload) - offset, &length) != 0 ||
        length >= capacities[field])
      return -1;
    for (unsigned k = 0U; k < 64U; ++k) {
      if (k > length)
        break;
      dest[field][k] = base[offset + k];
    }
    offset += length + 1U;
  }
  if (!nxm_service_path(candidate->path) ||
      (candidate->registry[0] != '\0' &&
       strncmp(candidate->registry, "srv.", 4U) != 0))
    return -1;
  return offset == sizeof(message->payload) ||
                 base[offset] == '\0' ? 0 : -1;
}

static int nxm_spawn(const struct nxm_slot *slot) {
  if (slot->args[0] == '\0')
    return (int)spawn_caps(slot->path, PLVL_ROOT, CAP_ALL);
  char args[sizeof(slot->args)];
  for (unsigned i = 0U; i < sizeof(args); ++i)
    args[i] = slot->args[i];
  char *argv[3] = {(char *)slot->path, NULL, NULL};
  unsigned argc = 1U, offset = 0U;
  for (unsigned i = 0U; i < sizeof(args); ++i) {
    char ch = args[i];
    if (ch == ' ' || ch == '\0') {
      if (offset < i) {
        if (argc >= 3U)
          return -EINVAL;
        argv[argc++] = args + offset;
        args[i] = '\0';
      }
      offset = i + 1U;
    }
    if (ch == '\0')
      break;
  }
  return spawn_args(slot->path, (int)argc, argv);
}

static void nxm_publish(const struct nxm_slot *slot, unsigned id) {
  char slotkey[40], value[48], pidbuf[24];
  int k = snprintf(slotkey, sizeof(slotkey), "system.manage.slot.%02u", id);
  int v = snprintf(value, sizeof(value), "%d:%u:%u", slot->pid,
                   slot->generation, (unsigned)slot->active);
  int p = snprintf(pidbuf, sizeof(pidbuf), "%d", slot->pid);
  if (k < 0 || k >= (int)sizeof(slotkey) ||
      v < 0 || v >= (int)sizeof(value) ||
      p < 0 || p >= (int)sizeof(pidbuf))
    return;
  if (OS1_registry_set(slotkey, value) != 0 ||
      (slot->registry[0] != '\0' &&
       OS1_registry_set(slot->registry, pidbuf) != 0))
    print("[NXMANAGE][WARN] registry publication failed\n");
}

static int nxm_start_slot(struct nxm_slot *slot, unsigned id,
                          uint64_t now_ms) {
  int pid = nxm_spawn(slot);
  if (pid <= 0)
    return pid;
  slot->pid = pid;
  slot->started_ms = now_ms;
  ++slot->generation;
  if (slot->mode != NXMANAGE_MODE_LOCK) {
    uint64_t budget = slot->mode == NXMANAGE_MODE_ONCE
                          ? NXMANAGE_ONCE_TIMEOUT_MS
                          : NXMANAGE_ASYNC_TIMEOUT_MS;
    slot->deadline_ms = now_ms > UINT64_MAX - budget
                            ? UINT64_MAX : now_ms + budget;
  }
  slot->last_result = pid;
  slot->active = 1;
  nxm_publish(slot, id);
  return pid;
}

static int nxm_matching(const struct nxm_slot *old,
                        const struct nxm_slot *candidate) {
  return old->mode == candidate->mode &&
         strcmp(old->path, candidate->path) == 0 &&
         strcmp(old->args, candidate->args) == 0 &&
         strcmp(old->registry, candidate->registry) == 0;
}

static int nxm_register(struct nxm_state *state,
                        const struct ipc_message *message) {
  unsigned id = (unsigned)message->data1;
  uint32_t mode = (uint32_t)message->data2;
  if (id >= NXMANAGE_SLOTS ||
      (mode != NXMANAGE_MODE_LOCK && mode != NXMANAGE_MODE_ONCE &&
       mode != NXMANAGE_MODE_ASYNC_ONCE))
    return -EINVAL;
  struct nxm_slot candidate = {0};
  candidate.mode = mode;
  if (nxm_unpack(message, &candidate) != 0)
    return -EINVAL;
  struct nxm_slot *slot = &state->slots[id];
  if (slot->active) {
    if (!nxm_matching(slot, &candidate))
      return -EPERM;
    if (mode == NXMANAGE_MODE_ONCE && !slot->once_finished) {
      slot->ack_target = state->init_pid;
      return 0; /* completion ACK deferred without blocking PID1 */
    }
    return slot->once_finished ? slot->last_result : slot->pid;
  }
  *slot = candidate;
  slot->active = 1; /* lock remains registered even if spawn fails */
  slot->backoff_ms = NXMANAGE_BASE_MS;
  int result = nxm_start_slot(slot, id, os1_mono_ns() / UINT64_C(1000000));
  if (result > 0 && mode == NXMANAGE_MODE_ONCE) {
    slot->ack_target = state->init_pid;
    return 0; /* ACK on actual exit, never on spawn-only */
  }
  if (result <= 0 && mode == NXMANAGE_MODE_LOCK) {
    slot->queued = 1;
    slot->due_ms = os1_mono_ns() / UINT64_C(1000000) +
                   slot->backoff_ms;
    print("[NXMANAGE][WARN] initial service spawn queued for retry\n");
  }
  return result;
}

/* One-time authenticated capability grant: a public name is NEVER enough
 * to obtain a private management SEND right. PID is kernel stamped on IPC.
 * The new init instance must have allocated a handle table before HELLO. */
static void nxm_auth_init(struct nxm_state *state, int send_handle) {
  for (unsigned work = 0U; work < NXMANAGE_QUEUE_BUDGET; ++work) {
    struct ipc_message hello = {0};
    if (OS1low_ipc_try_recv(-1, &hello) != 0)
      return;
    if (hello.from != state->init_pid ||
        hello.type != NXMANAGE_REQ_HELLO ||
        hello.data1 != NXMANAGE_PROTO_VERSION) {
      print("[NXMANAGE][WARN] unauthorized authority handshake denied\n");
      continue;
    }
    long granted = OS1low_cap_grant(hello.from, send_handle, OS1_RIGHT_WRITE);
    struct ipc_message reply = {0};
    reply.type = NXMANAGE_REP_HELLO;
    reply.data1 = (uint64_t)granted;
    reply.data2 = NXMANAGE_PROTO_VERSION;
    if (OS1low_ipc_send(hello.from, &reply) != 0)
      print("[NXMANAGE][ERROR] failed to deliver authority grant\n");
  }
}

static void nxm_reply(int target_pid, unsigned slot, int result) {
  struct ipc_message response = {0};
  response.type = NXMANAGE_REP_REGISTER;
  response.data1 = slot;
  response.data2 = (uint64_t)(int64_t)result;
  if (OS1low_ipc_send(target_pid, &response) != 0)
    print("[NXMANAGE][WARN] failed to reply to init\n");
}

static void nxm_drain(struct nxm_state *state, int port) {
  for (unsigned work = 0U; work < NXMANAGE_QUEUE_BUDGET; ++work) {
    struct ipc_message request = {0};
    long result = OS1_port_try_recv(port, &request);
    if (result == -EAGAIN)
      return;
    if (result != (long)sizeof(request)) {
      print("[NXMANAGE][ERROR] buffered port receive failed\n");
      return;
    }
    if (request.from != state->init_pid || request.type != NXMANAGE_REQ_REGISTER) {
      print("[NXMANAGE][WARN] rejected unauthorized management request\n");
      continue;
    }
    int status = nxm_register(state, &request);
    if (status != 0)
      nxm_reply(request.from, (unsigned)request.data1, status);
  }
}

static void nxm_schedule(struct nxm_slot *slot, uint64_t now_ms) {
  uint64_t uptime = now_ms - slot->started_ms;
  if (uptime >= NXMANAGE_STABLE_MS)
    slot->backoff_ms = NXMANAGE_BASE_MS;
  else if (slot->backoff_ms < NXMANAGE_CEILING_MS) {
    uint32_t doubled = slot->backoff_ms * 2U;
    slot->backoff_ms = doubled < NXMANAGE_CEILING_MS ?
                       doubled : NXMANAGE_CEILING_MS;
  }
  slot->pid = 0;
  slot->queued = 1;
  slot->due_ms = now_ms + slot->backoff_ms;
}

/* Non-blocking completion of a real ONE-SHOT child. PID1 continues to
 * consume management messages, scan all locks and run the user-mode work
 * quantum while a dependency or a long mdebug suite executes. */
static void nxm_check_once(struct nxm_slot *slot, unsigned id,
                           uint64_t now_ms) {
  if (slot->pid <= 0 || slot->once_finished)
    return;
  int status = 0;
  int pid = slot->pid;
  int polled = waitpid(pid, &status, WNOHANG);
  if (polled == 0 && now_ms < slot->deadline_ms)
    return;
  int outcome = -EIO;
  if (polled == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0)
    outcome = pid;
  if (polled == 0) {
    if (kill_process(pid) < 0)
      print("[NXMANAGE][ERROR] one-shot deadline kill failed\n");
    print("[NXMANAGE][ERROR] one-shot deadline exhausted\n");
  }
  slot->pid = 0;
  slot->once_finished = 1;
  slot->last_result = outcome;
  slot->active = outcome > 0 ? 1 : 0;
  nxm_publish(slot, id);
  if (slot->mode == NXMANAGE_MODE_ONCE && slot->ack_target > 0) {
    nxm_reply(slot->ack_target, id, outcome);
    slot->ack_target = 0;
  }
  if (outcome < 0)
    print("[NXMANAGE][ERROR] one-shot child failed\n");
  else if (printf("[NXMANAGE] one-shot slot %u complete PID %d\n",
                  id, pid) < 0)
    print("[NXMANAGE][ERROR] one-shot diagnostic output failed\n");
}

static void nxm_supervise(struct nxm_state *state, uint64_t now_ms) {
  for (unsigned id = 0U; id < NXMANAGE_SLOTS; ++id) {
    struct nxm_slot *slot = &state->slots[id];
    if (!slot->active)
      continue;
    if (slot->mode != NXMANAGE_MODE_LOCK) {
      nxm_check_once(slot, id, now_ms);
      continue;
    }
    if (slot->pid > 0) {
      int status = 0;
      int result = waitpid(slot->pid, &status, WNOHANG);
      if (result == 0)
        continue;
      nxm_schedule(slot, now_ms);
      nxm_publish(slot, id);
      print("[NXMANAGE][WARN] supervised child exited: restart queued\n");
    }
    if (slot->queued && now_ms >= slot->due_ms) {
      slot->queued = 0;
      if (nxm_start_slot(slot, id, now_ms) <= 0)
        nxm_schedule(slot, now_ms);
    }
  }
}

static void nxm_supervise_init(struct nxm_state *state, uint64_t now_ms) {
  if (state->init_pid > 0) {
    int status = 0;
    if (waitpid(state->init_pid, &status, WNOHANG) == 0)
      return;
    state->init_pid = 0;
    state->init_due_ms = now_ms + state->init_backoff_ms;
    if (state->init_backoff_ms < NXMANAGE_CEILING_MS)
      state->init_backoff_ms *= 2U;
    print("[NXMANAGE][WARN] initializer exited: retry queued\n");
  }
  if (state->init_pid == 0 && now_ms >= state->init_due_ms) {
    state->init_pid = (int)spawn_caps(state->init_path, PLVL_ROOT, CAP_ALL);
    if (state->init_pid > 0) {
      char value[20];
      if (snprintf(value, sizeof(value), "%d", state->init_pid) > 0 &&
          OS1_registry_set("system.manage.init_pid", value) != 0)
        print("[NXMANAGE][WARN] init PID publication failed\n");
      if (printf("[NXMANAGE] initializer PID %d\n", state->init_pid) < 0)
        print("[NXMANAGE][ERROR] initializer log output failed\n");
    } else {
      state->init_pid = 0;
      state->init_due_ms = now_ms + state->init_backoff_ms;
    }
  }
}

int main(void) {
  struct nxm_state state = {0};
  state.init_backoff_ms = NXMANAGE_BASE_MS;
  if (get_pid() != 1 || nxm_boot_init_path(state.init_path) != 0) {
    print("[NXMANAGE][ERROR] invalid PID1 authority or bootstrap config\n");
    return 1;
  }
  int port = OS1_port_create_private(NXMANAGE_PORT);
  int send_handle = port < 0 ? -1 : (int)OS1low_handle_create(
      OS1_NS_PORT, NXMANAGE_PORT, OS1_RIGHT_WRITE | OS1_RIGHT_TRANSFER,
      OBJ_TYPE_PORT);
  if (port < 0 || send_handle < 0 ||
      OS1_registry_set("system.manage.pid", "1") != 0) {
    print("[NXMANAGE][ERROR] failed to bind authority port/registry\n");
    return 1;
  }
  if (printf("[NXMANAGE] PID1 authority active, port %s\n",
             NXMANAGE_PORT) < 0)
    return 1;
  nxm_supervise_init(&state, os1_mono_ns() / UINT64_C(1000000));
  for (uint64_t epoch = 0U; epoch < UINT64_MAX; ++epoch) {
    nxm_auth_init(&state, send_handle);
    nxm_drain(&state, port);
    uint64_t now_ms = os1_mono_ns() / UINT64_C(1000000);
    nxm_supervise(&state, now_ms);
    nxm_supervise_init(&state, now_ms);
    OS1_sleep(NXMANAGE_POLL_MS);
  }
  return 1; /* finite service epoch exhausted */
}
