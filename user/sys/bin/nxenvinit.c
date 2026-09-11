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
 * VISIBLE CONFIRMATION: nxinit spawns nxntfy_srv BEFORE nxenvinit, so this
 * process can post a real desktop notification of its own outcome.  The
 * notification endpoint is polled for a bounded time; if notify_srv never
 * comes up, environment bootstrap has already happened and a missing popup
 * is not a reason to hang the boot sequence.
 *
 * UTMP: the file this process writes is described by include/api/utmp.h —
 * struct layout, field offsets, and the on-disk path name all live there,
 * and nothing in this file is allowed to hardcode either.  Whatever gnulib's
 * readutmp.c uses to parse the file is the same header, so the writer and the
 * reader cannot drift.  See create_utmp_file() below.
 */
#include "nxinfo.h"
#include "nxperm.h" /* nxperm_level_name(), OS1_identity() via os1.h */
#include <os1.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <utmp.h> /* include/api/utmp.h — struct utmp, USER_PROCESS, _PATH_UTMP */

#define NX_ENV_READY_KEY "system.env_ready"

/* Bounded wait for notify_srv's discovery key. 30 x 20ms = 600ms ceiling —
 * generous next to notify_srv's own startup, but never allowed to stall
 * init's boot path indefinitely if the service fails to come up at all. */
#define NX_NOTIFY_WAIT_ATTEMPTS 30
#define NX_NOTIFY_WAIT_MS 20

/*
 * registry_init_defaults - seed registry keys with real post-boot data.
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
   * Deliberately NOT seeded: TERM, USER, SHELL — no verified consumer. */
  OS1_registry_set("sys.env.HOME", "/home");
  OS1_registry_set("sys.env.PATH", "/bin:/sys/bin");
  OS1_registry_set("sys.env.TMPDIR", "/tmp");

  printf("[EnvInit] Registry defaults initialised.\n");
}

/* nx_path_exists — true solo se `path` è un FILE REGOLARE.  Una directory
 * con quel nome NON conta: `create_utmp_file` deve poter scrivere il record
 * e una directory non lo accetta; distinguere i due casi evita il bug per
 * cui un `mkdir -p` accidentale nel rootfs faceva credere a nxenvinit che
 * il file esistesse già e la scrittura veniva saltata. */
static int nx_path_exists(const char *path) {
  struct abi_stat as;
  if (_sys_stat(path, &as) != 0)
    return 0;
  return as.type != ABI_S_TYPE_DIR;
}

/*
 * nx_mkdir_p — crea ricorsivamente una directory e i suoi genitori.
 *
 * SYS_MKDIR è stato ritirato da Programme R1b; si passa per OBJ_CTL_MKDIR
 * su un handle MUTATE alla directory GENITORE.  Il verbo accetta UN SOLO
 * nome di componente (il kernel riverifica la cosa), quindi questa
 * funzione itera: per "/home/var/run" crea "home" in "/", poi "var" in
 * "/home", poi "run" in "/home/var".
 *
 * Idempotente: se il componente esiste già, OBJ_CTL_MKDIR fallisce e il
 * risultato viene ignorato — non è un errore per questo uso.  Il
 * versione precedente passava il RESTO del path ("var/run") a OBJ_CTL_MKDIR,
 * che accetta un componente singolo: ogni chiamata falliva silenziosamente
 * e la directory non veniva mai creata, il che è esattamente il motivo per
 * cui nxenvinit non riusciva a scrivere /home/var/run/utmp.
 */
static void nx_mkdir_p(const char *path) {
  if (!path || !*path)
    return;

  /* `prefix` è il path fino (ed escluso) al componente corrente;
   * all'inizio è "/" per un path assoluto, "" altrimenti.  Ad ogni
   * iterazione: crea `comp` dentro `prefix`, poi estende `prefix`. */
  char prefix[256];
  char comp[128];
  size_t pi = 0;

  const char *p = path;
  if (*p == '/') {
    prefix[pi++] = '/';
    p++;
  }
  prefix[pi] = '\0';

  while (*p) {
    /* Estrai il prossimo componente (fino al prossimo '/' o NUL). */
    size_t clen = 0;
    while (*p && *p != '/') {
      if (clen + 1 < sizeof(comp))
        comp[clen++] = *p;
      p++;
    }
    comp[clen] = '\0';
    if (*p == '/')
      p++;
    if (clen == 0)
      continue; /* slash consecutivi ("//" o trailing slash) */

    /* Crea comp dentro prefix.  Fallimento (dir già esistente) ignorato. */
    int h = OS1low_handle_create(OS1_NS_FS, prefix, OS1_RIGHT_MUTATE,
                                 OBJ_TYPE_FILE);
    if (h >= 0) {
      OS1_object_ctl(h, OBJ_CTL_MKDIR, (long)comp);
      OS1low_handle_close(h);
    }

    /* Estendi prefix: aggiungi '/' + comp, salvo il caso in cui prefix sia
     * già "/" (per non generare "//"). */
    if (pi == 1 && prefix[0] == '/') {
      if (pi + clen + 1 <= sizeof(prefix)) {
        memcpy(prefix + pi, comp, clen + 1);
        pi += clen;
      }
    } else {
      if (pi + clen + 2 <= sizeof(prefix)) {
        prefix[pi++] = '/';
        memcpy(prefix + pi, comp, clen + 1);
        pi += clen;
      }
    }
  }
}

/* mkdir_parent_of — crea la sola directory genitore di `path`, ricavandola
 * dal path stesso.  Un solo posto in cui la convenzione "il lettore userà
 * _PATH_UTMP, quindi il file va scritto lì" è espressa; se la macro cambia
 * un giorno, questa funzione segue senza doverla aggiornare. */
static void mkdir_parent_of(const char *path) {
  char dir[256];
  size_t dlen = strlen(path);
  if (dlen == 0 || dlen >= sizeof(dir))
    return;
  memcpy(dir, path, dlen + 1);
  char *slash = strrchr(dir, '/');
  if (!slash)
    return;
  *slash = '\0';
  if (dir[0])
    nx_mkdir_p(dir);
}

/*
 * create_utmp_file - create the utmp file `who`/`users` will read.
 *
 * THE ONE RULE this function follows, and the reason it is short: every
 * byte written here is described by include/api/utmp.h — the same header
 * gnulib's readutmp.c uses to PARSE the file.  Field names, field types,
 * the value of USER_PROCESS, the size of a record (sizeof(struct utmp)),
 * and the path (_PATH_UTMP) all come from that header.  Nothing in this
 * function redeclares a struct, invents a field, hardcodes a path, or
 * hand-copies a constant.  If the layout in the header changes, the file
 * on disk changes with it; if the path in the header changes, this writer
 * follows.  A reader and a writer that agree because they include the
 * same header cannot drift — which is what "populate correctly" means.
 *
 * Identity is the CALLER's OWN level (OS1_identity + nxperm_level_name),
 * never a hardcoded "root" — matching the file-header note above.
 *
 * One USER_PROCESS record, no BOOT_TIME: on this tree the session IS the
 * shell that nxinit spawns, and a second synthetic "boot" record would
 * only make `who` show a phantom user.
 *
 * Returns 1 if it created the file this run, 0 if it already existed.
 */
static int create_utmp_file(void) {
  if (nx_path_exists(_PATH_UTMP))
    return 0;

  int level = 0;
  unsigned int mask = 0;
  OS1_identity(&level, &mask);
  const char *who = nxperm_level_name(level);

  struct utmp rec;
  memset(&rec, 0, sizeof(rec));

  /* ut_type: USER_PROCESS (7) — the constant comes from <utmp.h>, never a
   * literal 7 written here; if the ABI changed the value the writer would
   * silently disagree with the reader and who.c would skip the record. */
  rec.ut_type = USER_PROCESS;

  rec.ut_pid = get_pid();

  /* ut_line: kernel device name WITHOUT the "/dev/" prefix (POSIX
   * convention).  "tty1" is 4 chars; UT_LINESIZE is 32. */
  strncpy(rec.ut_line, "tty1", sizeof(rec.ut_line) - 1);

  /* ut_id: fixed 4 bytes on glibc, NOT required to be NUL-terminated.
   * memcpy states "4 raw bytes" — strncpy would leave the tail as NULs
   * (harmless for an id shorter than 4, but the copy semantics here
   * matter, so say them plainly). */
  memcpy(rec.ut_id, "tty1", 4);

  /* ut_user: the level name.  strncpy's NUL-padding is what UT_NAMESIZE
   * implies — the trailing bytes of the name field ARE NUL on glibc. */
  strncpy(rec.ut_user, who, sizeof(rec.ut_user) - 1);

  /* ut_host: empty for a local login (memset already zeroed it; the
   * explicit strncpy documents the intent without changing the bytes). */
  strncpy(rec.ut_host, "", sizeof(rec.ut_host) - 1);

  /* ut_exit: only meaningful for DEAD_PROCESS, but zero it explicitly so
   * the record has no uninitialised tail beyond what memset covers. */
  rec.ut_exit.e_termination = 0;
  rec.ut_exit.e_exit = 0;

  rec.ut_session = 0;

  /* ut_tv: struct __utmp_timeval in the header is int32_t tv_sec /
   * int32_t tv_usec — the glibc LP64 on-disk layout, NOT time_t/long.
   * The int32_t truncation past 2038 is a property of that FORMAT, not a
   * bug here: gnulib's reader will interpret the same 8 bytes the same
   * way. */
  rec.ut_tv.tv_sec = (int32_t)time(NULL);
  rec.ut_tv.tv_usec = 0;

  memset(rec.ut_addr_v6, 0, sizeof(rec.ut_addr_v6));

  /* Parent directory derived from _PATH_UTMP itself. */
  mkdir_parent_of(_PATH_UTMP);

  if (file_write(_PATH_UTMP, &rec, sizeof(rec), 0) < 0) {
    printf("[EnvInit] WARN: could not create %s\n", _PATH_UTMP);
    return 0;
  }
  printf("[EnvInit] Created %s (identity: %s, %u bytes)\n", _PATH_UTMP, who,
         (unsigned)sizeof(rec));
  return 1;
}

/*
 * create_wtmp_file - an EMPTY wtmp.
 *
 * `last`/`lastlog` open wtmp unconditionally; a missing file is what
 * produced the "Function not implemented" message this pair of creates
 * exists to prevent.  Empty is the truthful representation of a system
 * whose sessions are not (yet) logged — a reader will see 0 records and
 * succeed.
 */
static void create_wtmp_file(void) {
  if (nx_path_exists(_PATH_WTMP))
    return;
  mkdir_parent_of(_PATH_WTMP);
  /* Zero-byte offset-0 write = create-and-truncate on this tree's FS. */
  if (file_write(_PATH_WTMP, "", 0, 0) < 0)
    printf("[EnvInit] WARN: could not create %s\n", _PATH_WTMP);
}

/*
 * ensure_coreutils_dirs - directories configmake.h promises exist
 * (RUNSTATEDIR="/var/run", part of LOCALSTATEDIR="/var").  Idempotent.
 *
 * Deliberately NOT the parent of _PATH_UTMP: those are created by
 * mkdir_parent_of(_PATH_UTMP) above, from the header's own value, so this
 * function never has to know where the utmp file lives.
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
 * wait_for_notify_srv - poll "srv.notify_pid" (published by init once
 * nxntfy_srv's window is up) for up to NX_NOTIFY_WAIT_ATTEMPTS *
 * NX_NOTIFY_WAIT_MS. Returns 1 once the key is present, 0 on timeout.
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
 * report_outcome - visible confirmation.  Posts one notification describing
 * what actually happened this run, so success or failure is observable from
 * the desktop without watching UART.  Best-effort: if notify_srv never shows
 * up, this silently degrades to the printf/UART trail rather than blocking
 * or failing the process.
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
  create_wtmp_file();
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