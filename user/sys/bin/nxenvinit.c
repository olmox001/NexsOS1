/*
 * user/sys/bin/nxenvinit.c
 * One-shot environment bootstrap.
 *
 * Extracted out of nxinit.c's main() (registry_init_defaults() +
 * create_utmp_file() used to run inline there, before the first spawn).
 * nxinit launches this as a SEPARATE, non-supervised process — one spawn() +
 * a poll-wait, no entry in the supervisor's spawn queue, no backoff, no
 * respawn-on-death.  It runs once to completion and exits.
 *
 * IDENTITY: this file names NO users.  There is no /etc/passwd, no hardcoded
 * "root" string anywhere below.  The one identity this process ever refers
 * to is ITS OWN, and it gets it the same way nxperm.h (the ASTRA
 * users/permissions foundation) already does: OS1_identity() — the kernel's
 * own per-process LEVEL (machine/root/user/guest, caps.h).
 *
 * IDEMPOTENCY: the first thing main() does is check the sentinel registry key
 * "system.env_ready".  If it is already "1", this process still reports via
 * notification (see below) but touches nothing else.
 *
 * VISIBLE CONFIRMATION (this revision): nxinit now spawns nxntfy_srv BEFORE
 * nxenvinit (see nxinit.c's ordering comment), specifically so nxenvinit can
 * post a real desktop notification of its own outcome instead of leaving
 * success/failure observable only on UART.  notify_srv registers its
 * "srv.notify_pid" registry key asynchronously, AFTER its window is created
 * (same race nxinit.c's own boot-notification comment documents) — so this
 * process polls for that key with a bounded retry instead of firing blind.
 * If notify_srv never comes up in that window, nxenvinit does NOT block
 * indefinitely on it: environment bootstrap has already happened by that
 * point, and a missing confirmation popup is not a reason to hang the boot
 * sequence.
 */
#include "nxinfo.h"
#include "nxperm.h" /* nxperm_level_name(), OS1_identity() via os1.h */
#include <os1.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define NX_ENV_READY_KEY "system.env_ready"

/* Bounded wait for notify_srv's discovery key. 30 x 20ms = 600ms ceiling —
 * generous next to notify_srv's own startup, but never allowed to stall
 * init's boot path indefinitely if the service fails to come up at all. */
#define NX_NOTIFY_WAIT_ATTEMPTS 30
#define NX_NOTIFY_WAIT_MS 20

/*
 * registry_init_defaults - seed registry keys with real post-boot data.
 * (Unchanged from the original nxinit.c extraction.)
 */
static void registry_init_defaults(void) {
  /* --- Sistema operativo e versione (da nxinfo.h) --- */
  OS1_registry_set("system.os", NXINFO_OS_NAME);
  OS1_registry_set("system.version", NXINFO_OS_VERSION);

  /* --- Architettura (da build flags) --- */
#if defined(__x86_64__) || defined(__amd64__)
  OS1_registry_set("system.arch", "amd64");
#elif defined(__aarch64__)
  OS1_registry_set("system.arch", "arm64");
#else
  OS1_registry_set("system.arch", "generic");
#endif

  /* --- Hostname --- */
  OS1_registry_set("system.hostname", "NeXs");

  /* --- Tempo di boot reale (secondi dall'epoch, per nxbar e orologio) --- */
  char boot_time[32];
  snprintf(boot_time, sizeof(boot_time), "%ld", OS1_time_now());
  OS1_registry_set("system.boot_time", boot_time);

  /* --- Aspetto del compositor (valori predefiniti) --- */
  OS1_registry_set("theme.color", "dark");
  OS1_registry_set("style.name", "minimal");
  OS1_registry_set("background.name", "blue");

  /* --- Pannello notifiche (inizialmente chiuso) --- */
  OS1_registry_set("sys.ntfy.panel_open", "0");

  /* --- Input --- */
  OS1_registry_set("mouse.sensitivity", "1.0");

  /* --- Environment: the MACHINE's defaults (Phase 17) ---
   * HOME    read by nxexec_resolve_path()'s '~' tier and nxlauncher.
   * PATH    the bare-name search inside nxexec_spawn_search.
   * TMPDIR  read by gnulib's real tempname() (mktemp, sort
   *         --temporary-directory, split, csplit).  Points at /tmp, which
   *         the rootfs already creates unconditionally.
   * Deliberately NOT seeded: TERM, USER, SHELL — no verified consumer
   * (unchanged rationale from the original nxinit.c). */
  OS1_registry_set("sys.env.HOME", "/home");
  OS1_registry_set("sys.env.PATH", "/bin:/sys/bin");
  OS1_registry_set("sys.env.TMPDIR", "/tmp");

  printf("[EnvInit] Registry defaults initialised.\n");
}

#define UT_LINESIZE 32
#define UT_NAMESIZE 32
#define UT_HOSTSIZE 256

struct exit_status {
  short e_termination;
  short e_exit;
};

struct nx_timeval {
  time_t tv_sec;
  long tv_usec;
};

struct utmp {
  short ut_type;
  pid_t ut_pid;
  char ut_line[UT_LINESIZE];
  char ut_id[4];
  char ut_user[UT_NAMESIZE];
  char ut_host[UT_HOSTSIZE];
  struct exit_status ut_exit;
  long ut_session;
  struct nx_timeval ut_tv;
  int32_t ut_addr_v6[4];
  char ut_pad[20];
};

static int nx_path_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

/*
 * nx_mkdir_p — crea ricorsivamente una directory e i suoi genitori.
 * SYS_MKDIR è stato ritirato da Programme R1b; si passa per OBJ_CTL_MKDIR
 * su un handle MUTATE alla directory genitore.
 */
static void nx_mkdir_p(const char *path) {
  char buf[256];
  strncpy(buf, path, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  for (char *p = buf + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      int h =
          OS1low_handle_create(OS1_NS_FS, buf, OS1_RIGHT_MUTATE, OBJ_TYPE_FILE);
      if (h >= 0) {
        OS1_object_ctl(h, OBJ_CTL_MKDIR, (long)(p + 1));
        OS1low_handle_close(h);
      }
      *p = '/';
    }
  }
  const char *slash = strrchr(buf, '/');
  if (slash && slash[1]) {
    char parent[256];
    int plen = slash - buf;
    if (plen == 0)
      plen = 1;
    memcpy(parent, buf, plen);
    parent[plen] = '\0';
    int h = OS1low_handle_create(OS1_NS_FS, parent, OS1_RIGHT_MUTATE,
                                 OBJ_TYPE_FILE);
    if (h >= 0) {
      OS1_object_ctl(h, OBJ_CTL_MKDIR, (long)(slash + 1));
      OS1low_handle_close(h);
    }
  }
}

/*
 * create_utmp_file - crea /etc/utmp con un record di login per il CALLER
 * REALE (nxenvinit stesso), letto via OS1_identity()/nxperm_level_name() —
 * mai una stringa "root" scritta a mano.  Necessario per `who`, `logname`
 * e altri comandi che leggono utmp.  Returns 1 if it created the file this
 * run, 0 if it already existed (used only to shape the notification text,
 * never to change behaviour).
 */
static int create_utmp_file(void) {
  if (nx_path_exists("/etc/utmp"))
    return 0;

  int level = 0;
  unsigned int mask = 0;
  OS1_identity(&level, &mask);
  const char *who =
      nxperm_level_name(level); /* "machine"/"root"/"user"/"guest" */

  struct utmp rec;
  memset(&rec, 0, sizeof(rec));

  rec.ut_type = 7; /* USER_PROCESS */
  rec.ut_pid = get_pid();
  strncpy(rec.ut_line, "tty1", sizeof(rec.ut_line) - 1);
  strncpy(rec.ut_id, "tty1", sizeof(rec.ut_id) - 1);
  strncpy(rec.ut_user, who, sizeof(rec.ut_user) - 1);
  rec.ut_tv.tv_sec = time(NULL);
  rec.ut_tv.tv_usec = 0;

  nx_mkdir_p("/etc");

  if (file_write("/etc/utmp", &rec, sizeof(rec), 0) < 0) {
    printf("[EnvInit] WARN: could not create /etc/utmp\n");
    return 0;
  }
  printf("[EnvInit] Created /etc/utmp (identity: %s)\n", who);
  return 1;
}

/*
 * ensure_coreutils_dirs - directories configmake.h promises exist
 * (RUNSTATEDIR="/var/run", part of LOCALSTATEDIR="/var").  Idempotent via
 * nx_mkdir_p, same as /etc above.
 */
static void ensure_coreutils_dirs(void) {
  nx_mkdir_p("/var/run");
  nx_mkdir_p("/var/tmp");
}

static int nxenvinit_already_ready(void) {
  char val[8];
  return OS1_registry_get(NX_ENV_READY_KEY, val, sizeof(val)) == 0 &&
         strncmp(val, "1", 2) == 0;
}

/*
 * wait_for_notify_srv - poll "srv.notify_pid" (published by init in
 * register_service_pid() once nxntfy_srv's window is up) for up to
 * NX_NOTIFY_WAIT_ATTEMPTS * NX_NOTIFY_WAIT_MS.  Returns 1 once the key is
 * present, 0 on timeout — this is the SAME race nxinit.c's own boot
 * notification already guards against, just from the other side.
 */
static int wait_for_notify_srv(void) {
  char pidbuf[16];
  for (int i = 0; i < NX_NOTIFY_WAIT_ATTEMPTS; i++) {
    if (OS1_registry_get("srv.notify_pid", pidbuf, sizeof(pidbuf)) == 0)
      return 1;
    OS1_sleep(NX_NOTIFY_WAIT_MS);
  }
  return 0;
}

/*
 * report_outcome - the visible confirmation this revision adds.  Posts one
 * notification describing what actually happened this run, so success or
 * failure is observable from the desktop without watching UART.  Best-effort:
 * if notify_srv never shows up, this silently degrades to the existing
 * printf/UART trail (unchanged) rather than blocking or failing the process.
 */
static void report_outcome(int already_ready, int registry_ok,
                           int utmp_created) {
  if (!wait_for_notify_srv()) {
    printf("[EnvInit] notify_srv not available; skipping confirmation "
           "popup (see UART log above for outcome)\n");
    return;
  }

  char msg[96];
  if (already_ready) {
    snprintf(msg, sizeof(msg), "Environment already initialised");
  } else if (registry_ok) {
    snprintf(msg, sizeof(msg), "Environment ready%s",
             utmp_created ? " (utmp created)" : "");
  } else {
    snprintf(msg, sizeof(msg), "Environment init finished with warnings");
  }
  notify("EnvInit", msg);
}

int main(void) {
  if (nxenvinit_already_ready()) {
    print("[EnvInit] Environment already initialised, nothing to do.\n");
    report_outcome(1, 1, 0);
    flush();
    return 0;
  }

  print("[EnvInit] Initialising system environment...\n");
  registry_init_defaults();
  int utmp_created = create_utmp_file();
  ensure_coreutils_dirs();

  int registry_ok = (OS1_registry_set(NX_ENV_READY_KEY, "1") == 0);
  if (!registry_ok)
    printf("[EnvInit] WARN: failed to publish %s=1 (will re-run on next "
           "launch)\n",
           NX_ENV_READY_KEY);

  print("[EnvInit] Environment initialised.\n");
  report_outcome(0, registry_ok, utmp_created);
  flush();
  return registry_ok ? 0 : 1;
}