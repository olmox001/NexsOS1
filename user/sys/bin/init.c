/*
 * user/sys/bin/init.c
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
  OS1_registry_set("style.name", "nexs");
  OS1_registry_set("background.name", "blue");

  /* --- Pannello notifiche (inizialmente chiuso) --- */
  OS1_registry_set("sys.ntfy.panel_open", "0");

  /* --- Input --- */
  OS1_registry_set("mouse.sensitivity", "1.0");

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
   * NOTE(USR-INIT-03): There is no respawn backoff or rate limit.  A crashing
   * service is respawned immediately on every supervisor iteration, which can
   * exhaust the process table before the system stabilises.
   */
  print("[Init] Entering supervisor loop\n");
  while (1) {
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
    int r = wait(pid_notify);
    if (r == pid_notify || r == -2) {
      print("[Init] Notification Server died! Respawning...\n");
      pid_notify = spawn("/sys/bin/nxntfy_srv");
      /* Refresh srv.notify_pid to the LIVE pid.  Without this, the registry key
       * still holds the corpse's pid and every notify_post returns -ESRCH until
       * a reboot.  Re-registering on respawn also overwrites any hijack a
       * malicious process may have written in the meantime. */
      if (pid_notify > 0)
        register_service_pid("srv.notify_pid", pid_notify);
    }
#ifndef LAUNCHER_AUTOSTART
    /* Respawn the shell when it is gone (freshly dead corpse OR already
     * reaped by the kernel).  spawn() assigns a fresh monotonic PID. */
    r = wait(pid_shell);
    if (r == pid_shell || r == -2) {
      print("[Init] NXShell terminated! Respawning...\n");
      pid_shell = spawn("/sys/bin/nxshell");
    }
#endif

    /* Respawn the dock if it dies (ROOT via the /sys/bin path preset, as
     * above).  Refresh srv.dock_pid on respawn — same corpse-pid hazard as
     * srv.notify_pid above. */
    r = wait(pid_nxui);
    if (r == pid_nxui || r == -2) {
      print("[Init] Dock died! Respawning...\n");
      pid_nxui = spawn("/sys/bin/nxui");
      if (pid_nxui > 0)
        register_service_pid("srv.dock_pid", pid_nxui);
    }
    /* Respawn nxbar if it dies */
    r = wait(pid_nxbar);
    if (r == pid_nxbar || r == -2) {
      print("[Init] nxbar died! Respawning...\n");
      pid_nxbar = spawn("/sys/bin/nxbar");
      if (pid_nxbar > 0)
        register_service_pid("srv.bar_pid", pid_nxbar);
    }

#if LAUNCHER_AUTOSTART
    /* Respawn the launcher.  Initial pid comes from the spawn above; if
     * LAUNCHER_AUTOSTART is disabled we never set pid_nxlauncher, so the
     * wait() on a stale handle would otherwise busy-loop — the #else keeps
     * the variable pinned to 0 so process_wait() returns -2 (gone). */
    r = wait(pid_nxlauncher);
    if (r == pid_nxlauncher || r == -2) {
      print("[Init] Launcher died! Respawning...\n");
      pid_nxlauncher = spawn("/sys/bin/nxlauncher");
      if (pid_nxlauncher > 0)
        register_service_pid("srv.launcher_pid", pid_nxlauncher);
    }
#endif

    /* NOTE(GFX-DYN-01): host display-change auto-resize is intentionally NOT
     * polled here — a per-iteration poll wastes cycles.  It will be re-added
     * event-driven (virtio-gpu display-change IRQ → deferred handler).  The
     * manual path (nxres / SYS_SET_DISPLAY_MODE) remains.  SYS_DISPLAY_POLL is
     * kept for that future event-driven caller. */

    /* SCHED-03: run any window closes the compositor deferred out of IRQ
     * context.  The close button enqueues the target pid (it cannot kill from
     * the input bottom-half — that UAF'd a process live on another CPU); init
     * performs the actual process_kill_subtree here, in process context, on
     * every supervisor pass.  ~50 ms latency (the sleep below) is imperceptible,
     * same as the notification/respawn model. */
    OS1_wm_drain();

    /* Sleep between supervisor passes instead of busy-spinning: with the real
     * kernel timer (SYS_NANOSLEEP) init is descheduled (~0% CPU) and woken by
     * its core's tick, so it can no longer monopolise a core while all children
     * are alive. 50 ms respawn latency is imperceptible. */
    OS1_sleep(50);
  }
  return 0;
}