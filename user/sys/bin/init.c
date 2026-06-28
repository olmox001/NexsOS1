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
 *   USR-SEC-01   (W3 SECURITY) notify_srv writes its PID to the global registry
 *                key "srv.notify_pid" with no authentication; any process can
 *                overwrite that key to hijack all system notifications.
 */
#include <os1.h>

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
  /* Spawn Notification Server */
  /* NOTE(USR-INIT-02): Hardcoded path.  init.cfg would provide this path but
   * is never read; the cfg also lists wrong paths (see file header). */
  printf("[Init] Spawning Notification Server...\n");
  int pid_notify = spawn("/sys/bin/nxntfy_srv");
  if (pid_notify > 0) {
    printf("[Init] Notification Server started (PID %d)\n", pid_notify);
  } else {
    print("[Init] Failed to spawn Notification Server!\n");
  }

  /* Spawn Shell */
  printf("[Init] Spawning Nxshell...\n");
  int pid_shell = spawn("/sys/bin/nxshell");
  if (pid_shell > 0) {
    printf("[Init] NXShell started (PID %d)\n", pid_shell);
  } else {
    print("[Init] Failed to spawn NXShell!\n");
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
  } else {
    print("[Init] Failed to spawn Dock!\n");
  }

  flush();

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

    /* Respawn the shell when it is gone (freshly dead corpse OR already
     * reaped by the kernel).  spawn() assigns a fresh monotonic PID. */
    int r = wait(pid_shell);
    if (r == pid_shell || r == -2) {
      print("[Init] NXShell terminated! Respawning...\n");
      pid_shell = spawn("/sys/bin/nxshell");
    }

    /* Check if notification server died and respawn. */
    r = wait(pid_notify);
    if (r == pid_notify || r == -2) {
      print("[Init] Notification Server died! Respawning...\n");
      pid_notify = spawn("/sys/bin/notify_srv");
    }

    /* Respawn the dock if it dies (ROOT via the /sys/bin path preset, as
     * above). */
    r = wait(pid_nxui);
    if (r == pid_nxui || r == -2) {
      print("[Init] Dock died! Respawning...\n");
      pid_nxui = spawn("/sys/bin/nxui");
    }

    /* NOTE(GFX-DYN-01): host display-change auto-resize is intentionally NOT
     * polled here — a per-iteration poll wastes cycles.  It will be re-added
     * event-driven (virtio-gpu display-change IRQ → deferred handler).  The
     * manual path (nxres / SYS_SET_DISPLAY_MODE) remains.  SYS_DISPLAY_POLL is
     * kept for that future event-driven caller. */

    /* Sleep between supervisor passes instead of busy-spinning: with the real
     * kernel timer (SYS_NANOSLEEP) init is descheduled (~0% CPU) and woken by
     * its core's tick, so it can no longer monopolise a core while all children
     * are alive. 50 ms respawn latency is imperceptible. */
    OS1_sleep(50);
  }

  return 0;
}
