/*
 * user/shell.c
 * Standalone Graphical Shell for AArch64 OS
 * Each process creates its own TTY window in the compositor.
 */
#include "proce.h"
#include <os1.h>

/* Window dimensions */
#define WIN_W 640
#define WIN_H 480

/* Colors */
#define COLOR_BG 0xFF1a1a2e
#define COLOR_FG 0xFFe0e0e0
#define COLOR_PROMPT 0xFF00ff88

/* Shell state */
static int my_window = -1;
static int running = 1;
static char cmd_buf[128];
static int cmd_len = 0;

/*
 * Compare strings
 */
static int str_eq(const char *a, const char *b) {
  while (*a && *b) {
    if (*a != *b)
      return 0;
    a++;
    b++;
  }
  return *a == *b;
}

/*
 * Redraw window background
 */
static void shell_redraw(void) {
  if (my_window < 0)
    return;

  /* Just clear the window */
  window_draw(my_window, 0, 0, WIN_W, WIN_H, COLOR_BG);

  /* Top accent line */
  window_draw(my_window, 0, 0, WIN_W, 2, COLOR_PROMPT);

  compositor_render();
}

/*
 * Process command
 */
static void process_command(void) {
  cmd_buf[cmd_len] = '\0';
  if (cmd_len == 0)
    return;

  print("\n");

  if (str_eq(cmd_buf, "help") || str_eq(cmd_buf, "?")) {
    print("\n\033[1;33mAvailable Commands:\033[0m\n");
    print("  help       - Show this help\n");
    print("  clear      - Clear screen\n");
    print("  time       - Show uptime\n");
    print("  demo       - Draw 2D shapes\n");
    print("  demo3d     - Launch 3D cube demo\n");
    print("  shell      - Open new shell window\n");
    print("  ps         - List processes\n");
    print("  ls [path]  - List directory contents\n");
    print("  cd <path>  - Change directory\n");
    print("  pwd        - Show current directory\n");
    print("  cat <path> - Show file contents\n");
    print("  kill <pid> - Kill process by PID\n");
    print("  about      - About this OS\n");
    print("  exit       - Exit shell\n");
  } else if (str_eq(cmd_buf, "clear")) {
    print("\033[2J\033[H");
    shell_redraw();
  } else if (str_eq(cmd_buf, "time")) {
    printf("Uptime: %d seconds (%x jiffies)\n", (int)(get_time() / 100),
           get_time());
  } else if (str_eq(cmd_buf, "demo")) {
    print("Drawing demo shapes in window...\n");
    for (int i = 0; i < 5; i++) {
      unsigned int colors[] = {0xFFff4444, 0xFF44ff44, 0xFF4444ff, 0xFFffff44,
                               0xFFff44ff};
      window_draw(my_window, 50 + i * 100, 100, 80, 80, colors[i]);
    }
    compositor_render();
  } else if (str_eq(cmd_buf, "demo3d")) {
    print("Launching 3D demo...\n");
    int pid = spawn("/bin/demo3d");
    if (pid > 0) {
      printf("Started demo3d with PID %d\n", pid);
    } else {
      print("Failed to start demo3d\n");
    }
  } else if (str_eq(cmd_buf, "shell")) {
    print("Opening new shell...\n");
    int pid = spawn("/bin/shell");
    if (pid > 0) {
      printf("Shell started. PID=%d\n", pid);
    } else {
      print("Failed to start shell\n");
    }
  } else if (str_eq(cmd_buf, "ps")) {
    proce_display_list(my_window);
  } else if (str_eq(cmd_buf, "ls") || (cmd_buf[0] == 'l' && cmd_buf[1] == 's' && cmd_buf[2] == ' ')) {
    const char *path = ".";
    if (cmd_buf[2] == ' ') path = &cmd_buf[3];
    char buf[1024];
    int len = list_dir(path, buf, sizeof(buf));
    if (len < 0) {
      printf("Error listing %s\n", path);
    } else {
      print(buf);
      print("\n");
    }
  } else if (str_eq(cmd_buf, "pwd")) {
    char buf[128];
    if (getcwd(buf, sizeof(buf)) == 0) {
      printf("%s\n", buf);
    } else {
      print("Error getting CWD\n");
    }
  } else if (cmd_buf[0] == 'c' && cmd_buf[1] == 'd' && (cmd_buf[2] == ' ' || cmd_buf[2] == '\0')) {
    const char *path = "/";
    if (cmd_buf[2] == ' ') path = &cmd_buf[3];
    if (chdir(path) != 0) {
      printf("cd: no such directory: %s\n", path);
    }
  } else if (cmd_buf[0] == 'k' && cmd_buf[1] == 'i' && cmd_buf[2] == 'l' &&
             cmd_buf[3] == 'l' && cmd_buf[4] == ' ') {
    /* Parse PID from "kill <pid>" */
    int pid = 0;
    for (int i = 5; cmd_buf[i] >= '0' && cmd_buf[i] <= '9'; i++) {
      pid = pid * 10 + (cmd_buf[i] - '0');
    }
    if (pid > 0) {
      printf("Killing PID %d...\n", pid);
      int result = kill_process(pid);
      if (result == 0) {
        print("Process terminated.\n");
      } else {
        print("Failed to kill process.\n");
      }
    } else {
      print("Usage: kill <pid>\n");
    }
  } else if (str_eq(cmd_buf, "about")) {
    print("\n\033[1;36mNeXs OS v0.0.1\033[0m\n");
    print("\033[33mGraphics:\033[0m Window Compositor + ANSI Terminal "
          "Emulator\n");
    print("\033[35mInput:\033[0m Interrupt-driven VirtIO Mouse/Keyboard\n");
    print("\033[32mLibrary:\033[0m POSIX-like userlib with printf support\n");
    print("\nSystem reported: OK\n");
  } else if (str_eq(cmd_buf, "exit")) {
    print("Exiting shell...\n");
    running = 0;
    exit(0);
  } else if (cmd_buf[0] == 'n' && cmd_buf[1] == 'o' && cmd_buf[2] == 't' &&
             cmd_buf[3] == 'i' && cmd_buf[4] == 'f' && cmd_buf[5] == 'y' &&
             cmd_buf[6] == ' ') {
    print("Notification sent.\n");
  } else if (cmd_buf[0] == 'c' && cmd_buf[1] == 'a' && cmd_buf[2] == 't' &&
             cmd_buf[3] == ' ') {
    char *path = &cmd_buf[4];
    char buf[256];
    int len = file_read(path, buf, sizeof(buf) - 1, 0);
    if (len < 0) {
      printf("Error reading %s\n", path);
    } else {
      buf[len] = '\0';
      printf("--- %s (%d bytes) ---\n", path, len);
      print(buf);
      if ((unsigned int)len >= sizeof(buf) - 1)
        print("\n...[truncated]...\n");
      else
        print("\n");
    }
  } else {
    /* Try spawn */
    char path[64];
    /* Prepend / if not present */
    if (cmd_buf[0] == '/')
      snprintf(path, sizeof(path), "%s", cmd_buf);
    else
      snprintf(path, sizeof(path), "/bin/%s", cmd_buf);

    int pid = spawn(path);
    if (pid > 0) {
      printf("Started %s (PID %d)\n", path, pid);
    } else {
      printf("Unknown command: %s\n", cmd_buf);
    }
  }

  cmd_len = 0;
}

/*
 * Main shell entry
 */
int main(void) {
  print("Shell: Alive\n");
  int pid = get_pid();
  /* Create a unique window for this shell instance */
  char title[32];
  sprintf(title, "Shell PID %d", pid);

  int x_off = (pid * 40) % 200;
  int y_off = (pid * 40) % 200;
  my_window = create_window(100 + x_off, 100 + y_off, WIN_W, WIN_H, title);

  if (my_window <= 0) {
    print("[Shell] Error creating window\n");
    exit(1);
  }

  shell_redraw();
  set_focus(get_pid());

  print("\n[Shell] TTY Window ");
  print_hex(my_window);
  printf(" active (PID %d).\n", get_pid());
  char cwd[128];
  getcwd(cwd, sizeof(cwd));
  printf("\033[32mshell\033[0m:\033[34m%s\033[0m> ", cwd);
  write(3, "shell> ", 7); /* Mirror to UART */

  char buf[2] = {0, 0};
  while (running) {
    long n = read(0, buf, 1);
    if (n <= 0)
      continue;

    char c = buf[0];
    if (c == '\n' || c == '\r') {
      process_command();
      if (running) {
        char prompt_cwd[128];
        getcwd(prompt_cwd, sizeof(prompt_cwd));
        printf("\033[32mshell\033[0m:\033[34m%s\033[0m> ", prompt_cwd);
      }
    } else if (c == '\b' || c == 127) {
      if (cmd_len > 0) {
        cmd_len--;
        print("\b \b");
      }
    } else if (c >= 32 && c < 127 && cmd_len < 126) {
      cmd_buf[cmd_len++] = c;
      buf[0] = c;
      buf[1] = 0;
      print(buf);
    }
  }

  exit(0);
  return 0;
}
