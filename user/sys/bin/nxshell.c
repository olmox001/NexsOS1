/*
 * user/sys/bin/nxshell.c
 * Interactive Graphical Shell
 *
 * Creates a compositor window that acts as a TTY.  Keyboard input arrives as
 * IPC and is drained through input_poll_event() (the same path nxui/nxsettings
 * use for resize and compositor-look notifications).  Commands are tokenized
 * in place and dispatched from a small builtin table; anything else is resolved
 * against /bin then /sys/bin via nxexec_spawn_search().
 *
 * Theme: palette follows nxres_theme_is_light() (nxres.h), refreshed on
 * INPUT_TYPE_LOOK_CHANGED (nxres_broadcast_look) and on a cheap per-loop poll
 * so every open shell instance tracks external theme changes even when more
 * than one is running.
 *
 * Resize: INPUT_TYPE_RESIZE -> OS1_window_resize() for a crisp terminal surface
 * (compositor reflows the cell grid via term_resize).
 */
#include "nxexec.h"
#include "nxperm.h"
#include "nxres.h"
#include <input.h>
#include <os1.h>
#include <stdlib.h>
#include <string.h>

#define CMD_MAX 256
#define SPAWN_PATH_MAX NXEXEC_PATH_MAX
#define MAX_ARGV 16
#define MIN_WIN_W 320
#define MIN_WIN_H 240

static int my_window = -1;
static int running = 1;
static char cmd_buf[CMD_MAX];
static int cmd_len = 0;
static int g_win_w = 640;
static int g_win_h = 480;
static int g_light = -1;

static uint32_t g_col_bg;
static uint32_t g_col_fg;
static uint32_t g_col_prompt;

static void shell_redraw_accent(void);

static void shell_load_colors(int light) {
  g_light = light;
  if (light) {
    g_col_bg = 0xFFF5F5F7u;
    g_col_fg = 0xFF1C1C1Eu;
    g_col_prompt = 0xFF007AFFu;
  } else {
    g_col_bg = 0xFF1A1A22u;
    g_col_fg = 0xFFE0E0E0u;
    g_col_prompt = 0xFF00FF88u;
  }
}

static void shell_check_theme(void) {
  int light = nxres_theme_is_light();
  if (light != g_light) {
    shell_load_colors(light);
    shell_redraw_accent();
  }
}

static void shell_redraw_accent(void) {
  if (my_window < 0)
    return;
  window_draw(my_window, 0, 0, g_win_w, 2, g_col_prompt);
  compositor_render();
}

static void shell_on_resize(int w, int h) {
  if (my_window < 0 || w <= 0 || h <= 0)
    return;
  if (w < MIN_WIN_W)
    w = MIN_WIN_W;
  if (h < MIN_WIN_H)
    h = MIN_WIN_H;
  if (w == g_win_w && h == g_win_h)
    return;
  OS1_window_resize(my_window, w, h);
  g_win_w = w;
  g_win_h = h;
  shell_redraw_accent();
}

/* Quote-aware tokenization lives in the libc (lib.c cmdline_split), shared
 * with every other command path; this keeps `sh -c` word-splitting identical
 * to the interactive prompt and strips shell-style quotes ("lua" -> lua). */
static int tokenize(char *s, char *argv[], int max) {
  return cmdline_split(s, argv, max);
}

static int spawn_search_args(int argc, char *argv[], char *out_path) {
  return nxexec_spawn_search(argc, argv, out_path, /*detached=*/0);
}

static void run_foreground(int pid) { nxexec_run_foreground(pid); }

static int skip_bin_entry(const char *name) {
  if (!name || !*name)
    return 1;
  const char *dot = strrchr(name, '.');
  if (!dot)
    return 0;
  return strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0 ||
         strcmp(dot, ".o") == 0 || strcmp(dot, ".old") == 0;
}

static void help_list_programs(const char *dir) {
  char buf[1024];
  int n = list_dir(dir, buf, sizeof(buf) - 1);
  if (n <= 0)
    return;

  buf[n] = '\0';

  char *save = NULL;
  int col = 0;

  for (char *tok = strtok_r(buf, " \t", &save); tok;
       tok = strtok_r(NULL, " \t", &save)) {

    if (skip_bin_entry(tok))
      continue;

    printf("%-16s", tok);

    col++;
    if (col == 4) {
      printf("\n");
      col = 0;
    }
  }

  if (col != 0)
    printf("\n");
}

static void cmd_help(void) {
  print("\n\033[1;33mBuilt-in commands:\033[0m\n");
  print("  help            Show this help\n");
  print("  clear           Clear the screen\n");
  print("  time            Show uptime\n");
  print("  ls [path]       List directory\n");
  print("  cd [path]       Change directory (/ if omitted)\n");
  print("  pwd             Print working directory\n");
  print("  cat <path>      Show file contents\n");
  print("  echo [text...]  Print arguments\n");
  print("  rm <path>       Remove a file\n");
  print("  mkdir <path>    Create a directory\n");
  print("  cp <src> <dst>  Copy file\n");
  print("  write <p> <txt> Write text to a file\n");
  print("  kill <pid>      Terminate a process\n");
  print("  focus <id>      Focus a window by id\n");
  print("  id              Show privilege level and capabilities\n");
  print("  about           About this system\n");
  print("  exec <prog>     Run a program with arguments\n");
  print("  exit            Close this shell\n");
  print("\n\033[1;33mPrograms in /sys/bin:\033[0m\n");
  help_list_programs("/sys/bin");
  print("\n\033[1;33mPrograms in Bin are not listed for brevity\033[0m\n");
  print("\nType any program name to run it (searches /bin, then /sys/bin).\n");
}

static void print_prompt(void) {
  char prompt_cwd[128];
  if (getcwd(prompt_cwd, sizeof(prompt_cwd)) != 0)
    prompt_cwd[0] = '\0';
  print("\r\n");
  printf("\033[32mNXShell\033[0m:\033[34m%s\033[0m> ", prompt_cwd);
}

static void process_command(void) {
  cmd_buf[cmd_len] = '\0';
  if (cmd_len == 0)
    return;

  char line[CMD_MAX];
  strncpy(line, cmd_buf, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';

  char *argv[MAX_ARGV];
  int argc = tokenize(line, argv, MAX_ARGV);
  if (argc == 0)
    return;

  const char *cmd = argv[0];

  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
    cmd_help();
  } else if (strcmp(cmd, "clear") == 0) {
    print("\033[2J\033[H");
    shell_redraw_accent();
  } else if (strcmp(cmd, "time") == 0 || strcmp(cmd, "uptime") == 0) {
    printf("Uptime: %d seconds (%ld ms)\n", (int)(get_time() / 1000),
           get_time());
  } else if (strcmp(cmd, "ls") == 0) {
    const char *path = argc >= 2 ? argv[1] : ".";
    char buf[1024];
    int len = list_dir(path, buf, sizeof(buf));
    if (len < 0)
      printf("ls: cannot list %s\n", path);
    else {
      print(buf);
      print("\n");
    }
  } else if (strcmp(cmd, "pwd") == 0) {
    char buf[128];
    if (getcwd(buf, sizeof(buf)) == 0)
      printf("%s\n", buf);
    else
      print("pwd: error\n");
  } else if (strcmp(cmd, "cd") == 0) {
    const char *path = argc >= 2 ? argv[1] : "/";
    if (chdir(path) != 0)
      printf("cd: no such directory: %s\n", path);
  } else if (strcmp(cmd, "cat") == 0) {
    if (argc < 2) {
      print("usage: cat <path>\n");
    } else {
      char buf[256];
      int len = file_read(argv[1], buf, sizeof(buf) - 1, 0);
      if (len < 0)
        printf("cat: cannot read %s\n", argv[1]);
      else {
        buf[len] = '\0';
        printf("--- %s (%d bytes) ---\n", argv[1], len);
        print(buf);
        if ((unsigned int)len >= sizeof(buf) - 1)
          print("\n...[truncated]...\n");
        else
          print("\n");
      }
    }
  } else if (strcmp(cmd, "echo") == 0) {
    for (int i = 1; i < argc; i++) {
      if (i > 1)
        print(" ");
      print(argv[i]);
    }
    print("\n");
  } else if (strcmp(cmd, "rm") == 0 || strcmp(cmd, "unlink") == 0) {
    if (argc < 2) {
      print("usage: rm <path>\n");
    } else if (OS1_fs_unlink(argv[1]) != 0) {
      printf("rm: cannot remove %s\n", argv[1]);
    }
  } else if (strcmp(cmd, "mkdir") == 0) {
    if (argc < 2) {
      print("usage: mkdir <path>\n");
    } else if (mkdir(argv[1], 0755) != 0) {
      printf("mkdir: cannot create directory %s\n", argv[1]);
    }
  } else if (strcmp(cmd, "cp") == 0) {
    if (argc < 3) {
      print("usage: cp <source> <destination>\n");
    } else {
      char buf[1024];

      int len = file_read(argv[1], buf, sizeof(buf), 0);
      if (len < 0) {
        printf("cp: cannot read %s\n", argv[1]);
      } else {
        int ret = OS1_fs_write(argv[2], buf, len, 0);

        if (ret < 0)
          printf("cp: cannot write %s\n", argv[2]);
      }
    }
  } else if (strcmp(cmd, "mv") == 0) {
    if (argc < 3) {
      print("usage: mv <source> <destination>\n");
    } else {
      char buf[1024];

      int len = file_read(argv[1], buf, sizeof(buf), 0);

      if (len < 0) {
        printf("mv: cannot read %s\n", argv[1]);
      } else {
        if (OS1_fs_write(argv[2], buf, len, 0) < 0) {
          printf("mv: cannot write %s\n", argv[2]);
        } else {
          if (OS1_fs_unlink(argv[1]) != 0)
            printf("mv: cannot remove original %s\n", argv[1]);
        }
      }
    }
  } else if (strcmp(cmd, "write") == 0) {
    if (argc < 3) {
      print("usage: write <path> <text...>\n");
    } else {
      char msg[192];
      int n = 0;
      for (int i = 2; i < argc && n < (int)sizeof(msg) - 1; i++) {
        if (i > 2 && n < (int)sizeof(msg) - 1)
          msg[n++] = ' ';
        for (const char *p = argv[i]; *p && n < (int)sizeof(msg) - 1; p++)
          msg[n++] = *p;
      }
      msg[n] = '\0';
      if (OS1_fs_write(argv[1], msg, n, 0) < 0)
        printf("write: failed on %s\n", argv[1]);
    }
  } else if (strcmp(cmd, "kill") == 0) {
    if (argc < 2) {
      print("usage: kill <pid>\n");
    } else {
      int pid = atoi(argv[1]);
      if (pid <= 0) {
        print("usage: kill <pid>\n");
      } else {
        printf("Killing PID %d...\n", pid);
        if (kill_process(pid) == 0)
          print("Process terminated.\n");
        else
          print("Failed to kill process.\n");
      }
    }
  } else if (strcmp(cmd, "focus") == 0) {
    if (argc < 2) {
      print("usage: focus <window-id>\n");
    } else {
      int id = atoi(argv[1]);
      if (id <= 0) {
        print("usage: focus <window-id>\n");
      } else {
        int r = OS1_window_focus(id);
        if (r == 0)
          printf("Focused window %d\n", id);
        else
          printf("focus failed (%d)\n", r);
      }
    }
  } else if (strcmp(cmd, "id") == 0 || strcmp(cmd, "whoami") == 0) {
    int level = 0;
    unsigned int mask = 0;
    OS1_identity(&level, &mask);
    char m[96];
    nxperm_mask_str(mask, m, (int)sizeof(m));
    printf("pid=%d level=%s caps=%s\n", get_pid(), nxperm_level_name(level), m);
  } else if (strcmp(cmd, "about") == 0) {
    print("\n\033[1;36mNeXs OS v0.0.5.0\033[0m\n");
    print("\033[33mGraphics:\033[0m Window Compositor + ANSI Terminal\n");
    print("\033[35mInput:\033[0m Interrupt-driven VirtIO Mouse/Keyboard\n");
    print("\033[32mLibrary:\033[0m POSIX-like userlib with printf support\n");
    print("\nSystem reported: OK\n");
  } else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
    print("Exiting NXShell...\n");
    running = 0;
    exit(0);
  } else if (strcmp(cmd, "exec") == 0) {
    if (argc < 2) {
      print("usage: exec <program> [args...]\n");
    } else {
      char path[SPAWN_PATH_MAX];
      int pid = spawn_search_args(argc - 1, &argv[1], path);
      if (pid > 0)
        run_foreground(pid);
      else
        printf("exec: not found: %s\n", argv[1]);
    }
  } else {
    char path[SPAWN_PATH_MAX];
    int pid = spawn_search_args(argc, argv, path);
    if (pid > 0)
      run_foreground(pid);
    else
      printf("Unknown command: %s\n", argv[0]);
  }

  cmd_len = 0;
}

static void shell_handle_key(unsigned char key, uint16_t scancode) {
  if (key == '\n' || key == '\r' || scancode == INPUT_KEY_ENTER) {
    print("\r\n");
    process_command();
    if (running)
      print_prompt();
    return;
  }

  if (key == '\b' || key == 127 || scancode == INPUT_KEY_BACKSPACE) {
    if (cmd_len > 0) {
      cmd_len--;
      print("\b \b");
    }
    return;
  }

  if (key >= 32 && key < 127 && cmd_len < CMD_MAX - 2) {
    cmd_buf[cmd_len++] = (char)key;
    char echo[2] = {(char)key, '\0'};
    print(echo);
  }
}

int main(int argc, char *argv[]) {
  /* Headless mode: `nxshell -c "<command>"` — the actual POSIX `sh -c`
   * contract, added because system() in lib.c finally has something to
   * invoke (fix for bug #193-adjacent). No window is created and nothing
   * is published to the registry: I/O flows through the inherited
   * controlling terminal, exactly like any other foreground job without
   * a window (see nxexec.h). */
  if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
    strncpy(cmd_buf, argv[2], sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';
    cmd_len = (int)strlen(cmd_buf);
    process_command();
    return 0; /* OS1 does not yet provide a real exit-status channel (see
               * system() in lib.c). A future kernel/registry enhancement
               * could propagate the actual exit status instead of always
               * returning 0. */
  }

  print("NXShell: Alive\n");
  int pid = get_pid();

  char cwd[128] = {0};
  getcwd(cwd, sizeof(cwd));
  if (strcmp(cwd, "/") == 0 || cwd[0] == '\0')
    chdir("/home");

  shell_load_colors(nxres_theme_is_light());

  {
    char pidbuf[16];
    snprintf(pidbuf, sizeof(pidbuf), "%d", pid);
    OS1_registry_set("srv.shell_pid", pidbuf);
  }

  char title[32];
  snprintf(title, sizeof(title), "NXShell PID %d", pid);

  long di = OS1_display_info();
  int sw = (int)((di >> 16) & 0xFFFF);
  int sh = (int)(di & 0xFFFF);
  if (sw <= 0)
    sw = 800;
  if (sh <= 0)
    sh = 600;

  g_win_w = (sw * 3) / 5;
  g_win_h = (sh * 11) / 20;
  if (g_win_w > 800)
    g_win_w = 800;
  if (g_win_h > 560)
    g_win_h = 560;
  if (g_win_w < MIN_WIN_W)
    g_win_w = MIN_WIN_W;
  if (g_win_h < MIN_WIN_H)
    g_win_h = MIN_WIN_H;

  int x_off = (pid * 40) % 200;
  int y_off = (pid * 40) % 200;
  int wx = (sw - g_win_w) / 2 + x_off;
  int wy = (sh - g_win_h) / 2 + y_off;
  if (wx < 0)
    wx = 100 + x_off;
  if (wy < 0)
    wy = 100 + y_off;

  my_window = create_window(wx, wy, g_win_w, g_win_h, title);
  if (my_window <= 0) {
    print("[NXShell] Error creating window\n");
    exit(1);
  }

  shell_redraw_accent();
  set_focus(get_pid());

  print("\n[NXShell] TTY Window ");
  print_hex(my_window);
  printf(" active (PID %d).\n", get_pid());
  print_prompt();
  write(3, "NXShell> ", 7);

  while (running) {
    shell_check_theme();

    input_event_t ev;
    while (input_poll_event(&ev) == 1) {
      if (ev.type == INPUT_TYPE_RESIZE)
        shell_on_resize(ev.resize.w, ev.resize.h);
      else if (ev.type == INPUT_TYPE_LOOK_CHANGED)
        shell_check_theme();
      else if (ev.type == INPUT_TYPE_KEYBOARD &&
               ev.keyboard.state == KEY_PRESSED)
        shell_handle_key(ev.keyboard.key, ev.keyboard.scancode);
    }

    OS1_sleep(10);
  }

  exit(0);
  return 0;
}
