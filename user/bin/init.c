/*
 * user/init.c
 * Init Process - System Initialization
 *
 * TODO: Read /init.cfg when file syscalls are implemented.
 * For now, uses hardcoded spawn list.
 */
#include <os1.h>

int main(void) {
  print("[Init] System Initialization Starting...\n");

  /* Spawn Notification Server */
  /* Spawn Notification Server */
  printf("[Init] Spawning Notification Server...\n");
  int pid_notify = spawn("/bin/notify_srv.elf");
  if (pid_notify > 0) {
    printf("[Init] Notification Server started (PID %d)\n", pid_notify);
  } else {
    print("[Init] Failed to spawn Notification Server!\n");
  }

  /* Spawn Shell */
  printf("[Init] Spawning Shell...\n");
  int pid_shell = spawn("/bin/shell");
  if (pid_shell > 0) {
    printf("[Init] Shell started (PID %d)\n", pid_shell);
  } else {
    print("[Init] Failed to spawn Shell!\n");
  }

  flush();

  /* Supervisor loop: Monitor and respawn critical processes */
  print("[Init] Entering supervisor loop\n");
  while (1) {
    /* Check if shell died and respawn */
    if (wait(pid_shell) == pid_shell) {
      print("[Init] Shell terminated! Respawning...\n");
      pid_shell = spawn("/bin/shell");
    }

    /* Check if notification server died and respawn */
    if (wait(pid_notify) == pid_notify) {
      print("[Init] Notification Server died! Respawning...\n");
      pid_notify = spawn("/bin/notify_srv.elf");
    }

    yield();
  }

  return 0;
}
