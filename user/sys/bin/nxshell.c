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
#include "nxjobs.h"
#include "nxline.h"
#include "nxperm.h"
#include "nxres.h"
#include <fcntl.h>
#include <input.h>
#include <os1.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CMD_MAX NXLINE_MAX
#define SPAWN_PATH_MAX NXEXEC_PATH_MAX
#define MAX_ARGV 16
#define MIN_WIN_W 320
#define MIN_WIN_H 240
#define NXSHELL_HISTORY_PATH "/home/.nxshell_history"

static int my_window = -1;
static int running = 1;
static int g_win_w = 640;
static int g_win_h = 480;
static int g_light = -1;

static struct nxline g_line;
static struct nxjobs g_jobs;

/* Exit status of the last foreground command — the value `nxshell -c` exits
 * with, so system()/os.execute() see the real result (Phase 2 gave us the
 * exit_code channel; this is what carries it out of the shell).  POSIX
 * conventions: 127 = command not found, 128+signal = killed. */
static int g_last_status = 0;

static void
print_prompt_only(void); /* fwd decl: used as nxline's repaint hook */
static void nxline_prompt_hook(void *ctx) {
  (void)ctx;
  print_prompt_only();
}

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

/* Redirection parsing lives in nxexec.h as EXECUTOR policy, so the shell, the
 * hosted terminal and the execution service all share ONE parser (maintainer
 * 2026-07-18: nxexec must handle everything the shell was handling).  The local
 * copy was deleted rather than aliased: two copies of a parser are exactly how
 * the graphical and terminal paths drifted apart in the first place. */

/*
 * spawn_search_args - resolve + spawn a command, honouring any `<`/`>`/`>>`/`2>`
 * redirections embedded in argv.  Centralised here so both the default command
 * path and the `exec` builtin get redirection for free.  With no redirections
 * this is exactly nxexec_spawn_search (nredir 0 -> plain spawn).
 */
static int spawn_search_args(int argc, char *argv[], char *out_path) {
  struct spawn_redir redir[SPAWN_MAX_REDIR];
  int fds[SPAWN_MAX_REDIR], nredir = 0, nfds = 0;
  if (nxexec_strip_redirections(&argc, argv, redir, &nredir, fds, &nfds) != 0)
    return -1; /* error already reported */
  if (argc == 0) {
    print("nxshell: no command\n"); /* e.g. "> out" with nothing to run */
    for (int i = 0; i < nfds; i++)
      close(fds[i]);
    return -1;
  }
  int pid =
      nxexec_spawn_search_redir(argc, argv, out_path, /*detached=*/0, redir,
                                nredir);
  /* The child owns its own dups now; drop our copies so the files close when
   * both sides are done (POSIX fork+dup2 lifecycle). */
  for (int i = 0; i < nfds; i++)
    close(fds[i]);
  return pid;
}

static int run_foreground(int pid) { return nxexec_run_foreground(pid); }

/* run_fg_job - foreground a freshly-spawned command; if the user Ctrl-Z's it
 * (NXEXEC_JOB_STOPPED), register it as a Stopped job so `jobs`/`bg`/`fg` can
 * pick it up (Phase 2 job control). */
static void run_fg_job(int pid, const char *cmd) {
  /* Capture the command's exit status on the way through (g_last_status is what
   * `nxshell -c` returns, so os.execute()/system() observe real failures). */
  if (nxexec_run_foreground_ex(pid, &g_last_status) == NXEXEC_JOB_STOPPED) {
    int id = nxjobs_add(&g_jobs, pid, cmd);
    nxjobs_mark_stopped(&g_jobs, nxjobs_find(&g_jobs, id));
    printf("[%d]+ Stopped   %s\n", id, cmd);
  }
}

/*
 * spawn_with_extra_redir - resolve + spawn a command, applying its own
 * `<`/`>`/`>>`/`2>` redirections PLUS one extra {extra_child_fd ← extra_parent_fd}
 * (a pipe end).  The CALLER owns extra_parent_fd and closes it afterwards; this
 * closes only the files it opened for the command's own redirections.  Returns
 * the PID or <= 0.  A negative extra_parent_fd means "no extra redirection".
 */
static int spawn_with_extra_redir(int argc, char *argv[], int extra_child_fd,
                                  int extra_parent_fd, char *out_path) {
  struct spawn_redir redir[SPAWN_MAX_REDIR];
  int fds[SPAWN_MAX_REDIR], nredir = 0, nfds = 0;
  if (nxexec_strip_redirections(&argc, argv, redir, &nredir, fds, &nfds) != 0)
    return -1;
  if (argc == 0) {
    for (int i = 0; i < nfds; i++)
      close(fds[i]);
    return -1;
  }
  if (extra_parent_fd >= 0 && nredir < SPAWN_MAX_REDIR) {
    redir[nredir].child_fd = extra_child_fd;
    redir[nredir].parent_fd = extra_parent_fd;
    redir[nredir].source_pid = 0; /* our own table */
    nredir++;
  }
  int pid = nxexec_spawn_search_redir(argc, argv, out_path, /*detached=*/0, redir,
                                      nredir);
  for (int i = 0; i < nfds; i++)
    close(fds[i]); /* our own `>`/`2>` files; NOT the caller's pipe end */
  return pid;
}

/*
 * run_pipeline - execute `LHS | RHS` (a single pipe, ASTRA OBJ_TYPE_PIPE).  The
 * RHS is the foreground stage; the LHS produces into the pipe.  Two producer
 * kinds are handled:
 *   - the `echo` BUILTIN: the shell writes its output straight into the pipe;
 *   - any other (SPAWNED) command: spawned with its stdout → the pipe write-end.
 * Each stage still honours its own `>`/`2>` redirections.  If the kernel cannot
 * make a pipe, fall back to a temp file for the echo case (increment-1
 * redirection) so the common `echo ... | cmd` still works.
 */
static void run_pipeline(int pipe_idx, int argc, char *argv[], int background) {
  argv[pipe_idx] = 0; /* terminate the LHS argv in place */
  int lhs_argc = pipe_idx;
  char **rhs_argv = &argv[pipe_idx + 1];
  int rhs_argc = argc - pipe_idx - 1;
  if (lhs_argc <= 0 || rhs_argc <= 0) {
    print("nxshell: syntax error near '|'\n");
    return;
  }
  int lhs_is_echo = (strcmp(argv[0], "echo") == 0);

  /* Start the consumer on a fresh pipe (executor policy).  A failure here is
   * the ONLY thing that selects the temp-file fallback — we no longer create a
   * probe pipe just to test the waters, which leaked one pipe per pipeline. */
  char rpath[SPAWN_PATH_MAX];
  int wfd = -1;
  int rpid = nxexec_spawn_pipe_consumer(rhs_argc, rhs_argv, rpath, &wfd);
  if (rpid <= 0) {
    /* No pipe/consumer: temp-file fallback for the echo producer. */
    if (!lhs_is_echo) {
      print("nxshell: cannot create pipe\n");
      return;
    }
    const char *tmp = "/home/.nxpipe";
    char buf[CMD_MAX];
    int n = 0;
    for (int i = 1; i < lhs_argc && n < (int)sizeof(buf) - 1; i++) {
      if (i > 1 && n < (int)sizeof(buf) - 1)
        buf[n++] = ' ';
      for (const char *p = argv[i]; *p && n < (int)sizeof(buf) - 1; p++)
        buf[n++] = *p;
    }
    if (n < (int)sizeof(buf))
      buf[n++] = '\n';
    if (OS1_fs_write(tmp, buf, n, 0) < 0) {
      printf("nxshell: cannot stage pipe temp %s\n", tmp);
      return;
    }
    int tfd = open(tmp, O_RDONLY);
    if (tfd < 0) {
      OS1_fs_unlink(tmp);
      return;
    }
    char tpath[SPAWN_PATH_MAX];
    int tpid = spawn_with_extra_redir(rhs_argc, rhs_argv, 0, tfd, tpath);
    close(tfd);
    if (tpid > 0) {
      run_fg_job(tpid, rhs_argv[0]);
    } else {
      g_last_status = 127; /* POSIX: command not found */
      printf("Unknown command: %s\n", rhs_argv[0]);
    }
    OS1_fs_unlink(tmp);
    return;
  }

  /* The consumer is already running on the pipe (started above); what stays
   * here is genuinely the shell's: WHO produces, and job tracking. */
  int lpid = -1;
  if (rpid > 0 && wfd >= 0) {
    if (lhs_is_echo) {
      /* Builtin producer: the shell writes the output itself — there is no
       * process to give the write end to. */
      for (int i = 1; i < lhs_argc; i++) {
        if (i > 1)
          write(wfd, " ", 1);
        write(wfd, argv[i], strlen(argv[i]));
      }
      write(wfd, "\n", 1);
    } else {
      char lpath[SPAWN_PATH_MAX];
      lpid = spawn_with_extra_redir(lhs_argc, argv, 1, wfd, lpath);
    }
  }
  if (wfd >= 0)
    close(wfd); /* ALWAYS: a retained write end leaves the consumer waiting for
                 * data that can never arrive */

  if (rpid > 0) {
    if (background) {
      int id = nxjobs_add(&g_jobs, rpid, rhs_argv[0]);
      printf("[%d] %d\n", id, rpid);
    } else {
      run_fg_job(rpid, rhs_argv[0]);
    }
  } else {
    g_last_status = 127; /* POSIX: command not found */
    printf("Unknown command: %s\n", rhs_argv[0]);
  }
  if (lpid > 0)
    OS1low_process_wait(lpid); /* reap the spawned producer */
}

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
  print("  jobs            List jobs ('+' = current, '-' = previous)\n");
  print("  fg [job]        Bring a job to the foreground\n");
  print("  bg [job]        Resume a stopped job in the background\n");
  print("  attach <pid>    Track an already-running process as a job\n");
  print("  disown [job]    Stop tracking a job (it keeps running)\n");
  print("  exit            Close this shell\n");
  print("\n\033[1;33mLine editing:\033[0m\n");
  print("  \xe2\x86\x90/\xe2\x86\x92 Home/End   move cursor    Delete   "
        "forward-delete\n");
  print("  \xe2\x86\x91/\xe2\x86\x93            history        Tab      "
        "complete command/path\n");
  print("  Ctrl-A/E        line start/end Ctrl-L   clear screen\n");
  print("  Ctrl-R          search history Ctrl-D   delete-fwd (or exit on "
        "empty line)\n");
  print("  <cmd> &         run in background\n");
  print("\n\033[1;33mRedirection & pipes:\033[0m\n");
  print("  cmd > file      stdout to file      cmd >> file   append\n");
  print("  cmd < file      stdin from file     cmd 2> file   stderr to file\n");
  print("  cmd | cmd       pipe stdout into the next command\n");
  print("\n\033[1;33mJob ids (POSIX):\033[0m\n");
  print("  (omitted)       the current job     %%  %+   the current job\n");
  print("  %N  or  N       job number N        %-       the previous job\n");
  print("  %str            command starts with str\n");
  print("  %?str           command contains str\n");
  print("\n\033[1;33mSequencing:\033[0m\n");
  print("  a ; b           run b after a, whatever a returned\n");
  print("  a && b          run b only if a succeeded\n");
  print("  a || b          run b only if a failed\n");
  print("\n\033[1;33mPrograms in /sys/bin:\033[0m\n");
  help_list_programs("/sys/bin");
  print("\n\033[1;33mPrograms in Bin are not listed for brevity\033[0m\n");
  print("\nType any program name to run it (searches /bin, then /sys/bin).\n");
}

/* print_prompt_only - just the colored prompt, no leading newline and no
 * line content. This is nxline's repaint hook (Ctrl-L, multi-match tab
 * completion) as well as the tail end of print_prompt() below — kept as one
 * function so the prompt text can never drift between the two call sites. */
static void print_prompt_only(void) {
  char prompt_cwd[128];
  if (getcwd(prompt_cwd, sizeof(prompt_cwd)) != 0)
    prompt_cwd[0] = '\0';
  printf("\033[32mNXShell\033[0m:\033[34m%s\033[0m> ", prompt_cwd);
}

static void print_prompt(void) {
  print("\r\n");
  print_prompt_only();
}

/*
 * run_command_line - execute ONE simple command (no `&&`/`||`/`;`).
 *
 * `line` is consumed in place by the tokeniser.  Sequencing is the caller's
 * job (process_command below); this function is the unit a connector decides
 * to run or skip, and it owns g_last_status for that unit.
 */
static void run_command_line(char *line) {
  char *argv[MAX_ARGV];
  int argc = tokenize(line, argv, MAX_ARGV);
  if (argc == 0)
    return;

  /* Each command reports its own status; builtins that print nothing bad leave
   * it at 0 (success).  Spawn paths overwrite it via run_fg_job(). */
  g_last_status = 0;

  /* Background job: a trailing standalone '&' token (nxjobs.h — job control
   * light, see its header comment on what is and isn't possible without a
   * kernel-side STOPPED state). Stripped before dispatch so every builtin
   * and spawn path below sees a clean argv, same as without '&'. */
  int background = 0;
  if (argc > 1 && strcmp(argv[argc - 1], "&") == 0) {
    background = 1;
    argc--;
  }

  /* Pipeline `LHS | RHS`: handled BEFORE builtin dispatch so a builtin producer
   * (echo) can feed a spawned consumer.  Single pipe (main.lua's `echo | lua`);
   * only the first `|` is split. */
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "|") == 0) {
      run_pipeline(i, argc, argv, background);
      return;
    }
  }

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
    print("\n\033[1;36mNeXs OS v0.0.5.2\033[0m\n");
    print("\033[33mGraphics:\033[0m Window Compositor + ANSI Terminal\n");
    print("\033[35mInput:\033[0m Interrupt-driven VirtIO Mouse/Keyboard\n");
    print("\033[32mLibrary:\033[0m POSIX-like userlib with printf support\n");
    print("\nSystem reported: OK\n");
  } else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
    print("Exiting NXShell...\n");
    running = 0;
    exit(0);
  } else if (strcmp(cmd, "jobs") == 0) {
    nxjobs_poll(&g_jobs);
    nxjobs_print(&g_jobs);
  } else if (strcmp(cmd, "fg") == 0) {
    nxjobs_poll(&g_jobs); /* don't resolve against jobs that already finished */
    int slot = nxjobs_resolve(&g_jobs, argc >= 2 ? argv[1] : (const char *)0);
    if (slot < 0) {
      /* Distinguish the two failures: with no operand the shell had nothing to
       * choose from, which is not the same as being handed a job id that does
       * not exist. */
      if (argc >= 2)
        printf("fg: %s: no such job\n", argv[1]);
      else
        print("fg: no current job\n");
    } else {
      printf("%s\n", g_jobs.slot[slot].cmd);
      if (g_jobs.slot[slot].state == NXJOB_STOPPED)
        nxjobs_cont(&g_jobs, slot); /* resume before foregrounding */
      if (run_foreground(g_jobs.slot[slot].pid) == NXEXEC_JOB_STOPPED) {
        nxjobs_mark_stopped(&g_jobs, slot); /* Ctrl-Z'd again: current job */
        printf("[%d]+ Stopped   %s\n", g_jobs.slot[slot].id,
               g_jobs.slot[slot].cmd);
      } else {
        nxjobs_reap(&g_jobs, slot);
      }
    }
  } else if (strcmp(cmd, "bg") == 0) {
    nxjobs_poll(&g_jobs);
    int slot = nxjobs_resolve(&g_jobs, argc >= 2 ? argv[1] : (const char *)0);
    if (slot < 0) {
      if (argc >= 2)
        printf("bg: %s: no such job\n", argv[1]);
      else
        print("bg: no current job\n");
    } else if (g_jobs.slot[slot].state != NXJOB_STOPPED)
      print("bg: job already running\n");
    else if (nxjobs_cont(&g_jobs, slot) == 0)
      printf("[%d] %s &\n", g_jobs.slot[slot].id, g_jobs.slot[slot].cmd);
    else
      print("bg: failed to resume\n");
  } else if (strcmp(cmd, "attach") == 0) {
    /*
     * attach <pid> - adopt an ALREADY-RUNNING process into this shell's job
     * table (Phase 4: "a separated process is NOT killed with its parent, but
     * jobs still tracks it").  This is the re-attach half of that model; the
     * detach half is `disown` below.
     *
     * Authority (kernel, sys_handle_create OS1_NS_PROC): TRACKING needs only a
     * WAIT/READ capability, which any live pid grants — so `jobs` status and
     * exit reporting work for any process, including one this shell never
     * spawned (a dock-launched app re-homed to another ancestor).  fg/bg
     * additionally need kill authority (self / descendant / privileged), so
     * they succeed for our own descendants and report a failure otherwise
     * rather than pretending.
     */
    if (argc < 2) {
      print("usage: attach <pid>\n");
    } else {
      int pid = atoi(argv[1]);
      int w = (pid > 0) ? OS1low_process_wait(pid) : -2;
      if (pid <= 0)
        print("attach: invalid pid\n");
      else if (w == -2)
        printf("attach: no such process: %d\n", pid);
      else if (w > 0)
        printf("attach: process %d has already exited\n", pid);
      else {
        /* Name it the SAME way the bar and dock do (Phase 3 identity). */
        char nm[NXJOBS_CMD_MAX];
        if (!nxexec_lookup_identity(pid, nm, (int)sizeof(nm)))
          snprintf(nm, sizeof(nm), "pid %d", pid);
        int id = nxjobs_add(&g_jobs, pid, nm);
        if (id < 0)
          print("attach: job table full\n");
        else
          printf("[%d] %d %s\n", id, pid, nm);
      }
    }
  } else if (strcmp(cmd, "disown") == 0) {
    /* disown [%N] - stop tracking a job WITHOUT killing it: the process keeps
     * running, separated from this shell (the inverse of `attach`). */
    int slot = nxjobs_resolve(&g_jobs, argc >= 2 ? argv[1] : (const char *)0);
    if (slot < 0) {
      print("disown: no such job\n");
    } else {
      printf("[%d] %d %s disowned\n", g_jobs.slot[slot].id,
             g_jobs.slot[slot].pid, g_jobs.slot[slot].cmd);
      nxjobs_reap(&g_jobs, slot); /* drop the slot; the process lives on */
    }
  } else if (strcmp(cmd, "exec") == 0) {
    if (argc < 2) {
      print("usage: exec <program> [args...]\n");
    } else {
      char path[SPAWN_PATH_MAX];
      int pid = spawn_search_args(argc - 1, &argv[1], path);
      if (pid <= 0) {
        g_last_status = 127; /* POSIX: command not found */
        printf("exec: not found: %s\n", argv[1]);
      } else if (background) {
        int id = nxjobs_add(&g_jobs, pid, argv[1]);
        printf("[%d] %d\n", id, pid);
      } else {
        run_fg_job(pid, argv[1]);
      }
    }
  } else {
    char path[SPAWN_PATH_MAX];
    int pid = spawn_search_args(argc, argv, path);
    if (pid <= 0) {
      g_last_status = 127; /* POSIX: command not found */
      printf("Unknown command: %s\n", argv[0]);
    } else if (background) {
      int id = nxjobs_add(&g_jobs, pid, argv[0]);
      printf("[%d] %d\n", id, pid);
    } else {
      run_fg_job(pid, argv[0]);
    }
  }
}

/* ---------------------------------------------------------------------------
 * Command SEQUENCING — `&&`, `||`, `;`  (plan stall S6, phase 17e)
 *
 * `cd X && cmd` used to perform only the `cd`.  nxshell had no sequencing at
 * all, so `&&` tokenised into an argument nobody ever looked at and the rest
 * of the line was dropped without a word.  Silently running HALF a command
 * line is worse than refusing it: it produced test runs that read as passes,
 * and the plan carried it as a documented trap for months with no owner.
 *
 * The split runs on the RAW line, BEFORE tokenisation, because the tokeniser
 * strips quotes — after it has run, `echo "a && b"` and `echo a && b` are
 * indistinguishable.  So the scanner has to repeat the tokeniser's quoting
 * rules (lib.c cmdline_split); the two must agree on what counts as quoted,
 * or a separator inside a string would split the line.
 *
 * Two-character operators are tested BEFORE their one-character prefixes, so
 * background (`cmd &`) and pipelines (`a | b`) keep their existing meaning.
 * ------------------------------------------------------------------------- */
#define SEQ_MAX 8   /* segments per line; a refusal, never a silent truncation */
#define SEQ_ALWAYS 0 /* `;` and the implicit connector before the first segment */
#define SEQ_AND 1    /* `&&` — run only if the previous segment SUCCEEDED */
#define SEQ_OR 2     /* `||` — run only if the previous segment FAILED */

/* split_sequence - cut `line` into segments at top-level `&&`/`||`/`;`.
 *
 * Writes NULs into `line`.  conn[i] is the connector that PRECEDES seg[i]
 * (conn[0] is always SEQ_ALWAYS).  Returns the segment count, or -1 if the
 * line has more than `max` of them. */
static int split_sequence(char *line, char *seg[], int conn[], int max) {
  int n = 1;
  char *p = line;
  seg[0] = p;
  conn[0] = SEQ_ALWAYS;

  while (*p) {
    if (*p == '"' || *p == '\'') { /* skip a quoted run, cmdline_split's rules */
      char q = *p++;
      while (*p && *p != q) {
        if (q == '"' && *p == '\\' && (p[1] == '"' || p[1] == '\\'))
          p++;
        p++;
      }
      if (*p)
        p++;
      continue;
    }

    int kind, len;
    if (p[0] == '&' && p[1] == '&') {
      kind = SEQ_AND;
      len = 2;
    } else if (p[0] == '|' && p[1] == '|') {
      kind = SEQ_OR;
      len = 2;
    } else if (p[0] == ';') {
      kind = SEQ_ALWAYS;
      len = 1;
    } else {
      p++;
      continue;
    }

    if (n >= max)
      return -1;
    *p = '\0'; /* terminate the segment that ends here */
    p += len;  /* and step past the whole operator */
    conn[n] = kind;
    seg[n] = p;
    n++;
  }
  return n;
}

static void process_command(void) {
  if (g_line.len == 0)
    return;

  char line[CMD_MAX];
  strncpy(line, g_line.buf, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';

  nxline_history_add(&g_line, g_line.buf);

  char *seg[SEQ_MAX];
  int conn[SEQ_MAX];
  int nseg = split_sequence(line, seg, conn, SEQ_MAX);
  if (nseg < 0) {
    printf("nxshell: more than %d commands in one line\n", SEQ_MAX);
    g_last_status = 2;
    nxline_reset(&g_line);
    return;
  }

  for (int i = 0; i < nseg && running; i++) {
    /* g_last_status is the shell's $?: run_fg_job() maintains it, and the
     * builtins set it on failure.  Short-circuiting reads it BEFORE the
     * segment runs, so a skipped segment leaves the chain's status intact —
     * which is what makes `a && b || c` behave: if a fails, b is skipped and
     * c still sees a's failure. */
    if (i > 0) {
      if (conn[i] == SEQ_AND && g_last_status != 0)
        continue;
      if (conn[i] == SEQ_OR && g_last_status == 0)
        continue;
    }
    run_command_line(seg[i]);
  }

  nxline_reset(&g_line);
}

static void shell_handle_key(unsigned char key, uint16_t scancode) {
  int r = nxline_feed_key(&g_line, key, scancode);

  if (r == NXLINE_SUBMIT) {
    print("\r\n");
    process_command(); /* reads g_line.buf, then calls nxline_reset() */
    if (running)
      print_prompt();
  } else if (r == NXLINE_EOF) {
    /* Ctrl-D on an empty line: same contract as typing "exit". */
    print("\r\n");
    print("Exiting NXShell...\n");
    running = 0;
    exit(0);
  } else if (r == NXLINE_CLEAR_SCREEN) {
    print("\033[2J\033[H");
    shell_redraw_accent();
    print_prompt_only();
    nxline_repaint_inline(&g_line);
  }
}

int main(int argc, char *argv[]) {
  /* Headless mode: `nxshell -c "<command>"` — the actual POSIX `sh -c`
   * contract, added because system() in lib.c finally has something to
   * invoke (fix for bug #193-adjacent). No window is created and nothing
   * is published to the registry: I/O flows through the inherited
   * controlling terminal, exactly like any other foreground job without
   * a window (see nxexec.h). */
  nxjobs_init(&g_jobs);
  nxline_init(&g_line, NXSHELL_HISTORY_PATH, nxline_prompt_hook, NULL);

  if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
    /* No history load/save for a one-shot -c invocation: it never reads
     * keyboard input, so nxline is used purely as a string holder here. */
    strncpy(g_line.buf, argv[2], sizeof(g_line.buf) - 1);
    g_line.buf[sizeof(g_line.buf) - 1] = '\0';
    g_line.len = (int)strlen(g_line.buf);
    process_command();
    /* Exit with the COMMAND's status, not a blanket 0.  Phase 2 added the
     * exit-status channel (per-process exit_code -> waitpid), and run_fg_job()
     * captures it into g_last_status; propagating it here is what makes
     * system()/os.execute() able to observe a failure at all — lua's test
     * suite asserts on exactly this (main.lua NoRun: `assert(not
     * os.execute(cmd))` for a command that must fail). */
    return g_last_status;
  }

  nxline_load_history(&g_line);

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
    nxjobs_poll(&g_jobs);

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