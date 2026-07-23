/*
 * user/sys/bin/nxinit.c
 * Process 1 — System Initializer and Service Supervisor
 *
 * This is the first userland process launched by the kernel after boot.
 * It is responsible for:
 *   1. Spawning the two mandatory system services (notify_srv, shell) in order.
 *   2. Sending the "Boot Complete" notification via IPC to notify_srv.
 *   3. Running a non-blocking supervisor loop that detects child exits and
 *      respawns the dead service immediately.
 *
 * Calling convention / runtime:
 *   _start (user/arch/<arch>/syscall.S) sets up the stack and calls main();
 *   main() never returns (the supervisor loop is infinite).
 *
 * Known issues:
 *   USR-INIT-01  (W1 REFINE) Supervisor is a correct non-blocking poll today;
 *                the PID-reuse hazard is NOT live — next_pid is a monotonic
 *                counter (kernel/sched/process.c:20,233); PIDs are never
 *                recycled.  A generation/owner check would be needed if PID
 *                recycling is ever introduced.
 *   USR-INIT-02  (W3 MISSING·DOC) init.cfg is never read despite the TODO
 *                comment below being stale: file_read (lib.c:72) is fully
 *                implemented and used elsewhere.  The paths in init.cfg
 *                (/notify_srv.elf, /nxshell) do not match the actual rootfs
 *                layout (/sys/bin/notify_srv, /sys/bin/nxshell).
 *   USR-INIT-03  (W2 BAD-IMPL) No respawn rate-limiting: a service that
 *                crashes immediately will be respawned in a tight loop,
 *                saturating the process table (MAX_PROCESSES=64, os1.h:16)
 *                with zombies until the system stalls.
 *   USR-SEC-01   (W3 SECURITY, RESIDUAL) srv.notify_pid has no authentication;
 *                any process can still overwrite it.  The hijack window is now
 *                bounded to ONE supervisor tick (~50 ms) because init
 *                re-publishes the live pid on every respawn (see
 *                register_notify_pid).  Full capability-based addressing
 *                (OBJ_TYPE_PROCESS handle) is the planned upgrade; see DIR-01
 *                M4.5 "IPC -> OBJ_TYPE_PORT".
 */
#include "nxinfo.h"
#include <os1.h>
#include <sys/wait.h> /* waitpid + status decode — Phase 2 supervisor logging */

/*
 * LAUNCHER_AUTOSTART - gate for spawning + supervising /sys/bin/nxlauncher
 * (LAUNCHER-01 #138).
 *
 * The launcher is a still-shaky feature: its always-on-bottom z-order and
 * tile grid are actively being tuned, and an accidental respawn during
 * development can spam the process table with half-broken launchers.  Flip
 * this to 0 to disable BOTH the initial spawn AND the supervisor respawn,
 * without commenting code or touching the rest of init.  Default ON; setting
 * it to 0 keeps the rest of the desktop (shell, dock, notifications)
 * unaffected.
 *
 * The launcher is spawned AFTER /sys/bin/nxui so the dock's window already
 * has a higher z-order at boot.  The launcher then calls OS1_window_lower()
 * itself (canonical ASTRA §6.7 WINDOW-LOWER verb) to restack itself at the
 * back of the z-order, sitting behind the dock and every user window.
 */
#define LAUNCHER_AUTOSTART 1

/*
 * registry_init_defaults - completa le chiavi di registro con dati reali.
 *
 * Chiamata DOPO tutti gli spawn iniziali. Il kernel ha già creato le chiavi
 * base con valori placeholder; init le sovrascrive con l'architettura, la
 * versione OS, il timestamp di boot reale e i default del compositor.
 * Ogni binario /sys/bin gira a PLVL_ROOT (preset per-path) e quindi ha
 * CAP_REG_WRITE come tutti gli altri servizi di sistema; init non è
 * l'unico scrittore, ma è quello che pubblica i valori reali post-boot.
 * registry_caller_owner() tratta MACHINE e ROOT come owner 0 (system), così
 * le chiavi restano scrivibili da qualunque servizio di sistema anche dopo
 * un respawn con PID diverso (kernel/lib/registry.c).
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
   * These are stored registry keys, not process state: they describe this
   * installation's layout, so they belong to the configuration namespace
   * (ASTRA §6.6) and outlive every process.  getenv() falls back here when a
   * process has no value of its own, which is why nothing has to copy PATH
   * into each new process at spawn.  Editing these reconfigures the machine
   * and needs CAP_REG_WRITE; a process changing its OWN copy does not.
   *
   * Seed ONLY what has a verified consumer.  A default with no reader is not
   * harmless documentation: it is a claim about the system that nothing keeps
   * true, and the first program to believe it finds out the hard way.
   *
   *   HOME  read by nxexec_resolve_path()'s '~' tier and nxlauncher — and
   *         nxexec.h already anticipates it ("a real per-user HOME later
   *         becomes a getenv() change alone").
   *   PATH  no reader YET: the bare-name search is hardcoded to /bin then
   *         /sys/bin inside nxexec_spawn_search.  That hardcoded list IS this
   *         key's meaning, and 17c makes the executor consume it, at which
   *         point Phase 12's move to /sys/services becomes a registry edit
   *         rather than a code edit.
   *
   * Deliberately NOT seeded:
   *   TERM    there is no terminal TYPE yet.  term.c implements a real
   *           ECMA-48 subset, but nothing names or describes that capability
   *           set, so TERM would advertise a type with nothing behind it (17d).
   *   USER    there is no user identity in this system.  Phase 11 owns that
   *           model and is blocked on its design doc; inventing a name here
   *           would pre-commit it.
   *   SHELL   no reader.  system() uses the STANDARD shell by POSIX, not
   * $SHELL. TMPDIR  no reader.  doom reads TEMP, and only under #if _WIN32.
   */
  OS1_registry_set("sys.env.HOME", "/home");
  OS1_registry_set("sys.env.PATH", "/bin:/sys/bin");

  printf("[Init] Registry defaults initialised.\n");
}

/*
 * register_service_pid - publish `pid` as the LIVE endpoint of a singleton
 * system service by writing it (decimal, NUL-terminated) to registry key
 * `key`.  Generalised from the original notify-only register_notify_pid
 * (NOTIFY-REG-01) so every long-lived /sys/bin service init supervises —
 * nxntfy_srv (srv.notify_pid), nxui/dock (srv.dock_pid), nxbar
 * (srv.bar_pid), nxlauncher (srv.launcher_pid) — is discoverable the same
 * way, and stays discoverable across a respawn.
 *
 * Called by init on the FIRST spawn AND on every RESPAWN of the service
 * (init.c main()).  Centralising the write in init fixes two bugs of the
 * old "server self-registers" model:
 *   1. A respawn left the key pointing at the corpse's pid — anything
 *      addressing the service by that pid (notify_post(), or nxres_h's
 *      theme-change ping, see nxres.h) resolved a dead pid and -ESRCH'd
 *      silently, so the messages disappeared forever after a single crash.
 *   2. Any process could overwrite the key (USR-SEC-01) and intercept
 *      messages meant for the service.  Re-registering on respawn
 *      overwrites any such hijack with the legitimate pid within one
 *      supervisor tick (~50 ms).
 *
 * Failure to write the registry key is non-fatal: init logs and continues.
 * Callers addressing the service will keep getting a stale/absent pid until
 * the key is present, but init's supervisor retries on the next respawn.
 */
static void register_service_pid(const char *key, int pid) {
  char pidbuf[16];
  int n = snprintf(pidbuf, sizeof(pidbuf), "%d", pid);
  if (n <= 0 || n >= (int)sizeof(pidbuf))
    return;
  if (OS1_registry_set(key, pidbuf) != 0)
    printf("[Init] WARN: failed to publish %s=%s\n", key, pidbuf);
}

/*
 * main - init entry point; never returns.
 *
 * Spawns notify_srv and shell, fires the "boot complete" notification, then
 * enters the supervisor loop.
 *
 * No parameters, no meaningful return value (return 0 is unreachable dead code
 * because the while(1) loop never exits).
 *
 * Side effects:
 *   - Creates two child processes via SYS_SPAWN.
 *   - Sends one IPC notify message to the notification server.
 *   - Calls SYS_FLUSH to push any buffered output before entering the loop.
 */
/* service_gone - Phase 2 supervisor probe. WNOHANG-waitpid a supervised
 * service; if it died, log HOW (clean exit code vs killed/faulted) and return
 * 1 so the caller respawns. 0 while still running. Replaces the ad-hoc
 * `wait(pid) == pid || -2` checks with one standard, status-aware seam. */
static int service_gone(int pid, const char *name) {
  int status = 0;
  int r = waitpid(pid, &status, WNOHANG);
  if (r == 0)
    return 0; /* still alive */
  if (r > 0 && WIFSIGNALED(status))
    printf("[Init] %s died (killed, sig %d)! Respawning...\n", name,
           WTERMSIG(status));
  else if (r > 0)
    printf("[Init] %s exited (code %d)! Respawning...\n", name,
           WEXITSTATUS(status));
  else
    printf("[Init] %s gone! Respawning...\n", name); /* reaped elsewhere */
  return 1;
}

int main(void) {
  print("[Init] System Initialization Starting...\n");

  /* Spawn Notification Server */
  /* NOTE(USR-INIT-02): Hardcoded path.  init.cfg would provide this path but
   * is never read; the cfg also lists wrong paths (see file header).
   * NOTE(NOTIFY-REG-01): init OWNS the srv.notify_pid registry key (see
   * register_service_pid() below).  nxntfy_srv no longer publishes its own PID
   * — otherwise a respawn leaves the key pointing at the corpse. */
  printf("[Init] Spawning Notification Server...\n");
  int pid_notify = spawn("/sys/bin/nxntfy_srv");
  if (pid_notify > 0) {
    printf("[Init] Notification Server started (PID %d)\n", pid_notify);
    register_service_pid("srv.notify_pid", pid_notify);
  } else {
    print("[Init] Failed to spawn Notification Server!\n");
  }

  /*
   * Spawn the EXECUTION SERVICE (nxexec --service, the R6 daemon).
   *
   * It MUST be started here and nowhere else, and the reason is the capability
   * model rather than convention: process_create_caps applies a MONOTONIC
   * creator clamp, so a child is never more privileged than its creator.  A
   * PLVL_USER client that spawned this service itself would get a PLVL_USER
   * service, and the daemon's two defining powers — OBJ_CTL_SETOWNER (handing a
   * spawned job back to the requesting client so job control still reaches it)
   * and taking a client's fds — are BOTH privileged-only.  Such a service would
   * answer requests but silently fail to delegate, breaking jobs/fg/bg in a way
   * that looks like a shell bug.  Started from init (PLVL_MACHINE) it lands at
   * the /sys/bin ROOT preset and is genuinely privileged.
   *
   * Clients therefore never spawn it: they CONNECT, by acquiring a send
   * capability to the OS1nx_exec port (ASTRA §6.4 "SRL services are supervised
   * ELF processes, exposed via IPC/capability").
   *
   * No register_service_pid() here on purpose: the port NAME is the discovery
   * endpoint now, which is strictly better than a pid key — it cannot go stale
   * across a respawn, and possession of the capability IS the authority.
   */
  printf("[Init] Spawning Execution Service (nxexec --service)...\n");
  char *execsvc_argv[2];
  execsvc_argv[0] = (char *)"/sys/bin/nxexec";
  execsvc_argv[1] = (char *)"--service";
  int pid_execsvc = spawn_args("/sys/bin/nxexec", 2, execsvc_argv);
  if (pid_execsvc > 0)
    printf("[Init] Execution Service started (PID %d)\n", pid_execsvc);
  else
    print("[Init] Failed to spawn Execution Service!\n");

  /* Spawn the dock (window-manager UI).  Plain spawn(): the ASTRA per-path
   * preset gives any /sys/bin binary ROOT authority (F1), which is exactly what
   * a window manager needs to acquire OBJ_TYPE_WINDOW control capabilities to
   * any app's window (focus / minimize / restore).  ROOT (not machine) keeps
   * the dock killable + respawnable like the shell. */
  printf("[Init] Spawning Dock (nxui)...\n");
  int pid_nxui = spawn("/sys/bin/nxui");
  if (pid_nxui > 0) {
    printf("[Init] Dock started (PID %d)\n", pid_nxui);
    /* srv.dock_pid: discovery endpoint for cross-process pushes (theme/style
     * change ping — nxres.h nxres_broadcast_look_changed) mirroring
     * srv.notify_pid, same reason: a respawn must not leave this pointing at
     * a corpse. */
    register_service_pid("srv.dock_pid", pid_nxui);
  } else {
    print("[Init] Failed to spawn Dock!\n");
  }

  /* Spawn Top Bar (nxbar) — classic X11-style status bar */
  printf("[Init] Spawning Top Bar (nxbar)...\n");
  int pid_nxbar = spawn("/sys/bin/nxbar");
  if (pid_nxbar > 0) {
    printf("[Init] nxbar started (PID %d)\n", pid_nxbar);
    register_service_pid("srv.bar_pid", pid_nxbar);
  } else {
    print("[Init] Failed to spawn nxbar!\n");
  }

  /* pid_nxlauncher is declared unconditionally so the supervisor's wait() call
   * below compiles regardless of LAUNCHER_AUTOSTART; when the gate is off we
   * pin it to 0 so process_wait() returns -2 (gone) and no respawn fires. */
  int pid_nxlauncher = 0;
#if LAUNCHER_AUTOSTART
  /* Spawn the launcher AFTER the dock so the dock's tiles stay on top of the
   * launcher's full-screen grid (LAUNCHER-01 #138 — always-on-bottom).  The
   * launcher also gets the per-path preset ROOT authority; it needs to
   * acquire WINDOW caps to spawn user apps from its tiles. */
  printf("[Init] Spawning Launcher (nxlauncher)...\n");
  pid_nxlauncher = spawn("/sys/bin/nxlauncher");
  if (pid_nxlauncher > 0) {
    printf("[Init] Launcher started (PID %d)\n", pid_nxlauncher);
    register_service_pid("srv.launcher_pid", pid_nxlauncher);
  } else {
    print("[Init] Failed to spawn Launcher!\n");
  }
#endif

  /* Spawn Shell */
  printf("[Init] Spawning Nxshell...\n");
  int pid_shell = spawn("/sys/bin/nxshell");
  if (pid_shell > 0) {
    printf("[Init] NXShell started (PID %d)\n", pid_shell);
  } else {
    print("[Init] Failed to spawn NXShell!\n");
  }

  flush();

  /* Completa il registro con i dati reali (timestamp, architettura, versione)
   * ora che tutti i servizi sono partiti e OS1_time_now() è significativo. */
  registry_init_defaults();

  /* The "Boot Complete" notification is sent from the supervisor loop below,
   * NOT here: notify() resolves the target from the registry key
   * "srv.notify_pid", which notify_srv writes only AFTER it has created its
   * window and is ready to recv(). Sending immediately raced that write — the
   * message went to the fallback PID and was lost, so the popup never appeared
   * at boot. We now wait for the key to exist, then send exactly once. */
  int boot_notified = 0;

  /* Supervisor loop: Monitor and respawn critical processes.
   *
   * wait(pid) maps to _sys_wait which calls kernel process_wait().
   * process_wait() is NON-BLOCKING and a PURE REPORTER:
   *   returns -1  if the named process is still alive,
   *   returns pid if the process is a corpse not yet drained by the
   *               scheduler's reaper,
   *   returns -2  if not found — either it never existed or it was ALREADY
   *               auto-reaped by schedule() (the kernel frees corpses on its
   *               own since the zombie-leak fix; a victim killed while
   *               parked is freed immediately and never appears as a
   *               waitable corpse).
   * Both pid and -2 therefore mean "child is gone": testing only ==pid made
   * the respawn a race against the kernel reaper (won on some boots/arches,
   * lost on others — the amd64 no-respawn report).
   *
   * NOTE(USR-INIT-01): This is a correct poll loop.  PIDs are monotonic
   * (next_pid, process.c) so a respawned service can never collide with the
   * surviving service's PID.  A failed spawn (pid <= 0) also yields -2 and
   * is retried on the next iteration.
   *
   * USR-INIT-03 (FIXED 2026-07-18): respawns were RATE LIMITED.  Previously a
   * service that crashed immediately was respawned on every supervisor
   * iteration; a single broken service therefore consumed the process table and
   * took the whole system down.  That is not theoretical — a one-line format
   * bug in the dock produced 28 respawns in a row and ended in a kernel panic,
   * which is how this was found.
   *
   * USR-INIT-04 (FIXED 2026-07-18): USR-INIT-03's fix was a flat LIFETIME
   * counter (respawns[slot] > 5 => gaveup[slot] = 1, forever).  That traded an
   * amplifying crash loop for a permanent one: six isolated crashes hours
   * apart — nothing in common, each one fully recovered from — permanently
   * killed the service with no further attempt, because the counter only ever
   * counted UP and "gaveup" never cleared.  For a MANDATORY service (dock,
   * execsvc, notify_srv) that is worse than the tight loop it replaced: at
   * least the tight loop kept the service reachable eventually.
   *
   * Fix: a small per-service SPAWN QUEUE with DECAYING exponential backoff
   * instead of a lifetime budget.
   *   - A dead service is not respawned inline; service_gone() enqueues a
   *     request (spawnq_request) with a "not before" deadline, and the queue
   *     is drained (spawnq_due) once per tick — this is what lets a boot storm
   *     of several simultaneous deaths get staggered instead of all firing
   *     spawn() back-to-back in the same 33 ms tick.
   *   - The backoff for a slot DOUBLES (capped at SPAWN_BACKOFF_MAX_MS) only
   *     when the service dies again before SPAWN_STABLE_MS of uptime — that is
   *     the actual crash-loop signature USR-INIT-03 was reacting to.
   *   - A service that survives >= SPAWN_STABLE_MS since its last (re)spawn is
   *     "recovered": its backoff resets to the base delay, so an occasional,
   *     unrelated crash months apart is never penalized by history.
   *   - There is no "gaveup" state.  A service stuck in a genuine crash loop
   *     is retried forever at the SPAWN_BACKOFF_MAX_MS ceiling — that ceiling
   *     is what bounds the amplification (28 respawns/tick -> at most one
   *     spawn per 30 s), without ever leaving a mandatory service dark.
   */
#define SPAWN_BACKOFF_BASE_MS                                                  \
  200 /* first retry: fast, matches the old immediate-respawn feel */
#define SPAWN_BACKOFF_MAX_MS                                                   \
  30000 /* ceiling: worst case one attempt every 30s, never zero */
#define SPAWN_STABLE_MS                                                        \
  10000 /* alive this long since last (re)spawn -> backoff resets */
#define SPAWN_QUEUE_MAX                                                        \
  8 /* one slot per supervised service, same indexing as before */

  struct spawn_req {
    int pending; /* 1 if a respawn is currently queued for this slot */
    unsigned long long
        not_before_ms; /* queue drains this request once now_ms >= this */
    int backoff_ms;    /* current backoff for this slot; doubles on fast repeat
                          deaths */
    unsigned long long
        last_spawn_ms; /* when this slot was last (re)spawned; 0 = never yet */
  };
  static struct spawn_req spawnq[SPAWN_QUEUE_MAX];
  for (int i = 0; i < SPAWN_QUEUE_MAX; i++) {
    spawnq[i].pending = 0;
    spawnq[i].not_before_ms = 0;
    spawnq[i].backoff_ms = SPAWN_BACKOFF_BASE_MS;
    spawnq[i].last_spawn_ms = 0;
  }
  unsigned long long now_ms = os1_mono_ns() / 1000000ULL;

  /* Seed last_spawn_ms for the services already brought up above, so a fast
   * crash-loop that starts right at boot is detected on its FIRST death
   * instead of getting one free "isolated failure" reset (last_spawn_ms==0 is
   * the sentinel spawnq_request() below treats as "never tracked yet"). */
  if (pid_notify > 0)
    spawnq[0].last_spawn_ms = now_ms;
  if (pid_execsvc > 0)
    spawnq[1].last_spawn_ms = now_ms;
  if (pid_nxui > 0)
    spawnq[2].last_spawn_ms = now_ms;
  if (pid_nxbar > 0)
    spawnq[3].last_spawn_ms = now_ms;
  if (pid_nxlauncher > 0)
    spawnq[4].last_spawn_ms = now_ms;
#ifndef LAUNCHER_AUTOSTART
  if (pid_shell > 0)
    spawnq[5].last_spawn_ms = now_ms;
#endif

  /* spawnq_enqueue - queue a (re)spawn for `slot` and choose its delay.
   *
   * `penalise` forces the backoff to GROW instead of measuring uptime.  It is
   * set when the spawn() ITSELF failed, which has no uptime to measure: with
   * the uptime rule alone, `now_ms - last_spawn_ms` is large in that case, so
   * every failed attempt reads as an "isolated failure", resets to the base
   * delay, and the slot is retried five times a second forever.  That is the
   * amplification USR-INIT-03 was written to stop, coming back through the
   * other door — a missing or unloadable binary is precisely the case that
   * never sets last_spawn_ms at all. */
#define spawnq_enqueue(slot, name, penalise)                                   \
  do {                                                                         \
    struct spawn_req *q = &spawnq[slot];                                       \
    unsigned long long uptime = now_ms - q->last_spawn_ms;                     \
    if (!(penalise) && (q->last_spawn_ms == 0 || uptime >= SPAWN_STABLE_MS)) { \
      q->backoff_ms =                                                          \
          SPAWN_BACKOFF_BASE_MS; /* isolated failure: full reset */            \
    } else {                                                                   \
      q->backoff_ms *= 2;                                                      \
      if (q->backoff_ms > SPAWN_BACKOFF_MAX_MS)                                \
        q->backoff_ms = SPAWN_BACKOFF_MAX_MS;                                  \
      printf("[Init] %s is not staying up — next attempt in %d ms "            \
             "(retrying, not giving up; see USR-INIT-04)\n",                   \
             name, q->backoff_ms);                                             \
    }                                                                          \
    q->pending = 1;                                                            \
    q->not_before_ms = now_ms + q->backoff_ms;                                 \
  } while (0)

  /* spawnq_idle - nothing queued for this slot, so it is worth PROBING.
   *
   * The probe must be gated on this, because service_gone() PRINTS on every
   * call and a queued request stays queued for its whole backoff — up to
   * SPAWN_BACKOFF_MAX_MS.  Probing unconditionally therefore reported the same
   * death once per 33 ms tick, up to ~900 identical lines per backoff cycle,
   * into the console the user is trying to read.  It also re-consulted a status
   * the kernel had already handed over: the Phase 9b reaped-status ring is
   * consume-on-read, so only the first probe carries real information and every
   * later one degrades to the anonymous "gone" branch. */
#define spawnq_idle(slot) (!spawnq[slot].pending)

  /* spawnq_due - true exactly once when a queued request's backoff has
   * elapsed; clears `pending` so the caller's spawn() only fires once per
   * request.  The caller MUST then call spawnq_mark_spawned() on success, or
   * spawnq_enqueue(..., 1) on failure — the request has already been consumed,
   * so a slot that does neither is never retried. */
#define spawnq_due(slot)                                                       \
  (spawnq[slot].pending && now_ms >= spawnq[slot].not_before_ms                \
       ? (spawnq[slot].pending = 0, 1)                                         \
       : 0)

#define spawnq_mark_spawned(slot) (spawnq[slot].last_spawn_ms = now_ms)

  print("[Init] Entering supervisor loop\n");
  while (1) {
    /* One clock read per tick: every spawnq_* call below uses this now_ms, so
     * a whole supervisor pass is internally consistent even though several
     * services may be checked/spawned in the same iteration. */
    now_ms = os1_mono_ns() / 1000000ULL;

    /* Fire the boot notification once notify_srv has registered its endpoint
     * (srv.notify_pid present in the registry). This makes the popup actually
     * appear at startup instead of racing notify_srv's registration. */
    if (!boot_notified) {
      char npid[16];
      if (OS1_registry_get("srv.notify_pid", npid, sizeof(npid)) == 0) {
        notify("System", "Boot Complete - NEXS");
        boot_notified = 1;
      }
    }

    /* Check if notification server died and respawn. */
    if (spawnq_idle(0) && service_gone(pid_notify, "Notification Server"))
      spawnq_enqueue(0, "Notification Server", 0);
    if (spawnq_due(0)) {
      pid_notify = spawn("/sys/bin/nxntfy_srv");
      /* Refresh srv.notify_pid to the LIVE pid.  Without this, the registry key
       * still holds the corpse's pid and every notify_post returns -ESRCH until
       * a reboot.  Re-registering on respawn also overwrites any hijack a
       * malicious process may have written in the meantime. */
      if (pid_notify > 0) {
        register_service_pid("srv.notify_pid", pid_notify);
        spawnq_mark_spawned(0);
      } else {
        spawnq_enqueue(0, "Notification Server", 1);
      }
    }
#ifndef LAUNCHER_AUTOSTART
    /* Respawn the shell when it is gone (freshly dead corpse OR already
     * reaped by the kernel).  spawn() assigns a fresh monotonic PID. */
    if (spawnq_idle(5) && service_gone(pid_shell, "NXShell"))
      spawnq_enqueue(5, "NXShell", 0);
    if (spawnq_due(5)) {
      pid_shell = spawn("/sys/bin/nxshell");
      if (pid_shell > 0)
        spawnq_mark_spawned(5);
      else
        spawnq_enqueue(5, "NXShell", 1);
    }
#endif

    /* Respawn the execution service.  Nothing to re-register: clients
     * rediscover it by PORT NAME, and the new instance re-publishes OS1nx_exec
     * by taking the receive right — so a respawn heals discovery automatically,
     * with no stale-pid window of the kind srv.notify_pid has to guard against.
     * (The new instance RETRIES that claim briefly, because the dead one's
     * receive handle is still being torn down when we get here — see
     * nxexec_service().)
     */
    if (spawnq_idle(1) && service_gone(pid_execsvc, "Execution Service"))
      spawnq_enqueue(1, "Execution Service", 0);
    if (spawnq_due(1)) {
      char *rargv[2];
      rargv[0] = (char *)"/sys/bin/nxexec";
      rargv[1] = (char *)"--service";
      pid_execsvc = spawn_args("/sys/bin/nxexec", 2, rargv);
      if (pid_execsvc > 0)
        spawnq_mark_spawned(1);
      else
        spawnq_enqueue(1, "Execution Service", 1);
    }

    /* Respawn the dock if it dies (ROOT via the /sys/bin path preset, as
     * above).  Refresh srv.dock_pid on respawn — same corpse-pid hazard as
     * srv.notify_pid above. */
    if (spawnq_idle(2) && service_gone(pid_nxui, "Dock"))
      spawnq_enqueue(2, "Dock", 0);
    if (spawnq_due(2)) {
      pid_nxui = spawn("/sys/bin/nxui");
      if (pid_nxui > 0) {
        register_service_pid("srv.dock_pid", pid_nxui);
        spawnq_mark_spawned(2);
      } else {
        spawnq_enqueue(2, "Dock", 1);
      }
    }
    /* Respawn nxbar if it dies */
    if (spawnq_idle(3) && service_gone(pid_nxbar, "nxbar"))
      spawnq_enqueue(3, "nxbar", 0);
    if (spawnq_due(3)) {
      pid_nxbar = spawn("/sys/bin/nxbar");
      if (pid_nxbar > 0) {
        register_service_pid("srv.bar_pid", pid_nxbar);
        spawnq_mark_spawned(3);
      } else {
        spawnq_enqueue(3, "nxbar", 1);
      }
    }

#if LAUNCHER_AUTOSTART
    /* Respawn the launcher.  Initial pid comes from the spawn above; if
     * LAUNCHER_AUTOSTART is disabled we never set pid_nxlauncher, so the
     * wait() on a stale handle would otherwise busy-loop — the #else keeps
     * the variable pinned to 0 so process_wait() returns -2 (gone). */
    if (spawnq_idle(4) && service_gone(pid_nxlauncher, "Launcher"))
      spawnq_enqueue(4, "Launcher", 0);
    if (spawnq_due(4)) {
      pid_nxlauncher = spawn("/sys/bin/nxlauncher");
      if (pid_nxlauncher > 0) {
        register_service_pid("srv.launcher_pid", pid_nxlauncher);
        spawnq_mark_spawned(4);
      } else {
        spawnq_enqueue(4, "Launcher", 1);
      }
    }
#endif

    /* NOTE(GFX-DYN-01): host display-change auto-resize is intentionally NOT
     * polled here — a per-iteration poll wastes cycles.  It will be re-added
     * event-driven (virtio-gpu display-change IRQ → deferred handler).  The
     * manual path (nxres / SYS_SET_DISPLAY_MODE) remains.  SYS_DISPLAY_POLL is
     * kept for that future event-driven caller. */

    /* SCHED-STACK-ISO (ASTRA DIR-02): DRIVE THE COMPOSITOR RENDER from here, in
     * process context, instead of the kernel timer IRQ.  Running the heavy
     * render from the tick nested it on an arbitrary interrupted task's kernel
     * stack and smashed live frames on other CPUs (the amd64 click/nanosleep
     * panic and the aarch64 current_chip #PF).  flush() ->
     * SYS_COMPOSITOR_RENDER runs the render on init's OWN kernel stack — never
     * nested.  The render is damage-clipped, so an idle pass with nothing dirty
     * is cheap.  ~30 FPS pacing (33 ms) matches the old tick cadence. */
    flush();

    /* Sleep between supervisor passes.  33 ms ≈ 30 FPS: init is the
     * compositor's frame pump now, so the desktop refresh cadence lives here.
     * With the real kernel timer (SYS_NANOSLEEP) init is descheduled between
     * frames (~0% CPU at idle) and woken by its core's tick. */
    OS1_sleep(33);
  }
  return 0;
}