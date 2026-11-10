/*
 * nxinit — declarative system startup policy in userland.
 *
 * The generic PID1 nxmanageDD extension reads its own init ELF from VFS and
 * runs this process as ROOT. Init reads a protected service manifest; it
 * declares order, one-shot dependencies and persistent locks over the
 * authenticated bounded manager port. It NEVER owns the restart queue or
 * process-reaping policy. The manager retains real process ownership.
 */
#include <nxmanage.h>
#include <os1.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { NXINIT_MANIFEST_BYTES = 2048, NXINIT_LINE_BYTES = 160,
       NXINIT_ACK_POLL_MS = 50, NXINIT_PORT_RETRIES = 40 };
/* Manifest is owned by the single boot transaction, not global state. */

static int nxinit_boot_profile(void) {
  char mode[8] = {0};
  int n = file_read(NXMANAGE_PROFILE_CONFIG, mode, sizeof(mode) - 1U, 0);
  if (n != 2 || mode[1] != '\n' || (mode[0] != '0' && mode[0] != '1')) {
    print("[Init][ERROR] malformed machine-only boot profile\n");
    return -1;
  }
  return mode[0] == '1' ? 1 : 0;
}

static int nxinit_port_claim(void) {
  /* Allocate an ordinary PROCESS READ handle, not a machine-control right.
   * This allocates the target handle table for the future kernel grant. */
  int root = (int)OS1low_handle_create(OS1_NS_PROC, "1",
                                      OS1_RIGHT_READ, OBJ_TYPE_PROCESS);
  if (root < 0)
    return root;
  if (OS1low_handle_close(root) != 0)
    return -EIO;
  struct ipc_message hello = {0};
  hello.type = NXMANAGE_REQ_HELLO;
  hello.data1 = NXMANAGE_PROTO_VERSION;
  if (OS1low_ipc_send(1, &hello) != 0)
    return -EIO;
  for (unsigned attempt = 0U; attempt < NXMANAGE_ACK_STEPS; ++attempt) {
    struct ipc_message reply = {0};
    if (OS1low_ipc_try_recv(-1, &reply) == 0) {
      if (reply.from != 1 || reply.type != NXMANAGE_REP_HELLO ||
          reply.data2 != NXMANAGE_PROTO_VERSION)
        continue;
      int handle = (int)reply.data1;
      long info = OS1low_cap_query(handle);
      if (handle < 0 || info < 0 ||
          OS1_CAPQ_TYPE(info) != OBJ_TYPE_PORT ||
          OS1_CAPQ_RIGHTS(info) != OS1_RIGHT_WRITE)
        return -EPERM;
      return handle;
    }
    OS1_sleep(NXINIT_ACK_POLL_MS);
  }
  return -EIO;
}

static int nxinit_pack(char *dst, unsigned cap, const char *path,
                       const char *args, const char *registry) {
  const char *fields[3] = {path, args, registry};
  unsigned at = 0U;
  for (unsigned i = 0U; i < 3U; ++i) {
    unsigned length = 0U;
    for (; length < 64U && fields[i][length] != '\0'; ++length) {
    }
    if (length >= cap || length + 1U > cap - at)
      return -1;
    for (unsigned k = 0U; k < 64U; ++k) {
      if (k > length)
        break;
      dst[at + k] = fields[i][k];
    }
    at += length + 1U;
  }
  return 0;
}

static int nxinit_ack(unsigned slot) {
  for (unsigned poll = 0U; poll < NXMANAGE_ACK_STEPS; ++poll) {
    struct ipc_message reply = {0};
    if (OS1low_ipc_try_recv(-1, &reply) == 0) {
      if (reply.from != 1 || reply.type != NXMANAGE_REP_REGISTER ||
          reply.data1 != slot) {
        print("[Init][WARN] ignored unrelated management reply\n");
        continue;
      }
      return (int)(int64_t)reply.data2;
    }
    OS1_sleep(NXINIT_ACK_POLL_MS);
  }
  print("[Init][ERROR] supervisor ACK deadline exceeded\n");
  return -EIO;
}

static int nxinit_declare(int port, unsigned slot, uint32_t mode,
                          const char *path, const char *args,
                          const char *registry) {
  struct ipc_message message = {0};
  message.type = NXMANAGE_REQ_REGISTER;
  message.data1 = slot;
  message.data2 = mode;
  if (nxinit_pack(message.payload, sizeof(message.payload), path,
                  args, registry) != 0)
    return -EINVAL;
  if (OS1_port_send(port, &message) != (long)sizeof(message))
    return -EIO;
  int pid = nxinit_ack(slot);
  if (pid <= 0)
    printf("[Init][WARN] service slot %u not running (result %d)\n", slot,
           pid);
  else
    printf("[Init] service slot %u started (PID %d)\n", slot, pid);
  return pid;
}

/* Parses one immutable manifest line, never changes daemon policy.
 * The three separators are mandatory so malformed lines cannot silently
 * change the interpretation of executable paths or registry authority. */
static int nxinit_line(int port, unsigned slot, char *line, int mdebug) {
  char *field[4] = {line, NULL, NULL, NULL};
  unsigned split = 1U;
  for (unsigned i = 0U; i < NXINIT_LINE_BYTES && line[i] != '\0'; ++i) {
    if (line[i] == '|') {
      if (split >= 4U)
        return -EINVAL;
      line[i] = '\0';
      field[split++] = line + i + 1U;
    }
  }
  if (split != 4U || field[1][0] != '/')
    return -EINVAL;
  uint32_t mode = 0U;
  if (strcmp(field[0], "lock") == 0)
    mode = NXMANAGE_MODE_LOCK;
  else if (strcmp(field[0], "once") == 0)
    mode = NXMANAGE_MODE_ONCE;
  else if (strcmp(field[0], "debug") == 0) {
    if (!mdebug)
      return 0; /* skipped by boot profile, not executed */
    mode = NXMANAGE_MODE_ASYNC_ONCE;
  } else {
    return -EINVAL;
  }
  int result = nxinit_declare(port, slot, mode, field[1], field[2], field[3]);
  if (result <= 0 && mode == NXMANAGE_MODE_ONCE)
    return -EIO; /* dependent services must not start after failed bootstrap */
  return result <= 0 ? 1 : 0; /* persistent lock can be retried by manager */
}

static int nxinit_manifest(int port, int mdebug) {
  char manifest[NXINIT_MANIFEST_BYTES];
  int n = file_read(NXMANAGE_SERVICES_CONFIG, manifest,
                    sizeof(manifest) - 1U, 0);
  if (n <= 0 || n >= (int)sizeof(manifest))
    return -EIO;
  manifest[n] = '\0';
  unsigned slot = 0U, start = 0U;
  for (unsigned i = 0U; i < NXINIT_MANIFEST_BYTES; ++i) {
    if (i > (unsigned)n)
      break;
    if (manifest[i] != '\n' && manifest[i] != '\0')
      continue;
    unsigned length = i - start;
    if (length >= NXINIT_LINE_BYTES)
      return -EINVAL;
    manifest[i] = '\0';
    if (length > 0U && manifest[i - 1U] == '\r')
      manifest[i - 1U] = '\0';
    char *line = manifest + start;
    if (line[0] != '\0' && line[0] != '#') {
      if (slot >= NXMANAGE_SLOTS)
        return -ENOSPC;
      int result = nxinit_line(port, slot, line, mdebug);
      if (result < 0)
        return result;
      ++slot; /* stable slot indices even when a debug case is skipped */
    }
    start = i + 1U;
  }
  if (printf("[Init] declared %u service slots to NXMANAGE\n", slot) < 0)
    return -EIO;
  return 0;
}

int main(void) {
  if (get_pid() == 1) {
    print("[Init][ERROR] initializer cannot run as machine PID1\n");
    return 1;
  }
  int mdebug = nxinit_boot_profile();
  if (mdebug < 0)
    return 1;
  int port = nxinit_port_claim();
  if (port < 0) {
    print("[Init][ERROR] NXMANAGE authority port unavailable\n");
    return 1;
  }
  if (printf("[Init] Declarative startup from %s (mdebug=%d)\n",
             NXMANAGE_SERVICES_CONFIG, mdebug) < 0)
    return 1;
  if (nxinit_manifest(port, mdebug) != 0) {
    print("[Init][ERROR] service manifest rejected or dependency failed\n");
    if (OS1low_handle_close(port) < 0)
      print("[Init][WARN] failed to close authority handle\n");
    return 1;
  }
  if (OS1low_handle_close(port) < 0)
    print("[Init][WARN] failed to close authority handle\n");
  OS1_sleep(1000);
  char notify_pid[24] = {0};
  if (OS1_registry_get("srv.notify_pid", notify_pid,
                       sizeof(notify_pid)) == 0)
    notify("System", "Boot Complete - NEXS");
  print("[Init] Configuration delivered; lifecycle owned by NXMANAGE\n");
  /* Stay available for authenticated reconfiguration protocol extensions.
   * No duplicate service-status polling or respawn scheduling in init. */
  for (uint64_t epoch = 0U; epoch < UINT64_MAX; ++epoch) {
    OS1_sleep(1000);
  }
  return 1; /* finite lifetime bound exhausted */
}
