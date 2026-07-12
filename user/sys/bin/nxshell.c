/*
 * user/sys/bin/nxshell.c
 * Interactive Graphical Shell
 *
 * Creates a compositor window that acts as a TTY.  Reads single characters
 * from fd 0 (keyboard input delivered as IPC by the kernel input driver),
 * accumulates them into cmd_buf, and dispatches on newline.
 *
 * Command dispatch is a linear if-else chain (process_command).  Commands
 * that need an argument parse character offsets directly from cmd_buf rather
 * than splitting tokens — the pattern `cmd_buf[2] == ' '` / `&cmd_buf[3]`
 * appears for ls, cd, cat, kill, notify.
 *
 * The nxshell accepts arbitrary PID arguments to kill without any privilege
 * check; see USR-SEC-02.
 *
 * Known issues:
 *   USR-SEC-02  (W3 SECURITY) kill <pid> accepts any decimal PID from user
 *               input and passes it directly to kill_process() with no
 *               capability check; any user can kill any system service.
 *   USR-SEC-03  (W3 WRONG-DESIGN) spawn() (and the fallback at line ~192)
 *               launches any ELF with full authority; no namespace or
 *               sandboxing constraint applies.
 *   USR-SHELL-01 (W2 BAD-IMPL) Command parsing uses hardcoded character
 *                offsets (e.g. cmd_buf[2]=='  ', &cmd_buf[3]) instead of
 *                token splitting; brittle and inconsistent across commands.
 *   USR-SHELL-02 (W2 MISSING) cmd_buf is 128 bytes, single-line only; no
 *                command history, no tab completion, no argument splitting.
 *   USR-BLOAT-01 (W2 BAD-IMPL·PERF) Every shell binary carries the full
 *                stb_image/stb_easy_font blob via lib.o (~500KB ELF).
 *   USR-BLOAT-02 (W2 BAD-IMPL) -g DWARF and -fno-omit-frame-pointer inflate
 *                the ELF; no --gc-sections or strip step.
 */
#include "nxexec.h"
#include <os1.h>

/* Window dimensions */
#define WIN_W 640
#define WIN_H 480

/* Colors */
#define COLOR_BG 0xFFFCFCFD
#define COLOR_FG 0xFFe0e0e0
#define COLOR_PROMPT 0xFF00ff88

/*
 * Nxshell state — module-level globals (one set per shell process since there
 * is no shared-library mechanism; each shell ELF has its own BSS).
 *
 * my_window: compositor window ID for this shell instance; -1 until created.
 * running:   cleared by the "exit" command to break the input loop.
 * cmd_buf:   accumulates the current line; NUL-terminated before dispatch.
 * cmd_len:   index of the next character to write in cmd_buf.
 *
 * NOTE(USR-SHELL-02): cmd_buf[128] limits line length to 127 printable chars
 * with no overflow protection beyond the `cmd_len < 126` guard in the input
 * loop; there is no history or tab-completion state.
 */
static int my_window = -1;
static int running = 1;
static char cmd_buf[128];
static int cmd_len = 0;

/*
 * str_eq - naive NUL-terminated string equality test.
 *
 * Returns 1 if 'a' and 'b' are identical, 0 otherwise.
 * Both pointers must be valid (no NULL guard).
 *
 * The standard strcmp/strncmp from lib.c/string.c could replace this;
 * the standalone copy predates the library import.
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
 * shell_redraw - repaint the window background.
 *
 * Fills the entire window with COLOR_BG (dark navy), then draws a 2-pixel
 * accent stripe at the top in COLOR_PROMPT (bright green).  Calls
 * compositor_render() to push the update to the screen.
 *
 * Guards against my_window < 0 (window not yet created); safe to call early
 * but does nothing until create_window() has succeeded.
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
 * Program launch helpers.  spawn_search_args() resolves a program name against
 * /bin then /sys/bin (absolute paths bypass the search) and hands the child an
 * argv vector via spawn_args(); tokenize() splits a command line into that
 * vector.  *out_path (size SPAWN_PATH_MAX) receives the last path tried.
 *
 * NOTE(USR-SEC-03): spawn_args() grants the new process full ambient authority;
 * no sandboxing applies regardless of the directory searched.
 */
/* Must match the shared launch layer's buffer contract: nxexec_spawn_search
 * writes up to NXEXEC_PATH_MAX bytes into out_path. */
#define SPAWN_PATH_MAX NXEXEC_PATH_MAX
#define MAX_ARGV 16

/*
 * tokenize - split a command line into argv in place (USR-SHELL-01 follow-up).
 *
 * Whitespace runs are collapsed; each token is NUL-terminated inside 's'
 * (which is mutated).  Returns argc (0..max); argv[i] point into 's'.  This is
 * the minimal splitter needed to pass arguments to spawned programs, e.g.
 * `kilo notes.txt` -> argv = {"kilo", "notes.txt"}.  No quoting/escaping yet.
 */
static int tokenize(char *s, char *argv[], int max) {
  int argc = 0;
  while (*s && argc < max) {
    while (*s == ' ')
      s++; /* skip leading spaces */
    if (!*s)
      break;
    argv[argc++] = s;
    while (*s && *s != ' ')
      s++; /* consume the token */
    if (*s)
      *s++ = '\0';
  }
  return argc;
}

/*
 * spawn_search_args - spawn argv[0] (probing /bin then /sys/bin) with argv.
 *
 * Probes /bin/<argv[0]> then /sys/bin/<argv[0]> and hands the child the full
 * argv vector via spawn_args().  argv[0] is the program name as typed; absolute
 * names (leading '/') bypass the search.  Returns the PID or <= 0 on failure.
 */
static int spawn_search_args(int argc, char *argv[], char *out_path) {
  /* Foreground (needs-shell) mode: the shell IS the child's ctty — the
   * launch logic itself lives in the shared nxexec layer (#193). */
  return nxexec_spawn_search(argc, argv, out_path, /*detached=*/0);
}

/*
 * process_command - parse and dispatch the accumulated line in cmd_buf.
 *
 * NUL-terminates cmd_buf at cmd_len, then matches against a linear chain of
 * if-else branches.  Returns early on an empty line.
 *
 * Dispatch strategy (NOTE USR-SHELL-01):
 *   - Fixed-word commands ("help", "clear", "time", etc.) use str_eq().
 *   - Commands with arguments ("ls", "cd", "cat", "kill", "notify", "exec")
 *     detect the command prefix by inspecting individual characters
 *     (cmd_buf[0..N]) and extract the argument via hardcoded byte offsets.
 *     There is no tokeniser.
 *   - Unknown tokens are tokenized and tried as ELF names via
 * spawn_search_args(), /bin/ then /sys/bin/ before reporting failure.
 *
 * On return, cmd_len is reset to 0 (erases the accumulated line).
 *
 * Side effects: writes to UART/window, may spawn processes, may call exit().
 */
/*
 * run_foreground - run a freshly-spawned child as a foreground shell job
 * (USR-TTY-01 #123, POSIX-like).
 *
 * A windowless CLI program writes its stdout into THIS shell's window (its
 * controlling terminal, resolved kernel-side), so it runs "in the shell".
 * We poll until it exits or the user presses Ctrl+C (ETX 0x03, delivered as
 * a keyboard IPC press), which kills it.  If the child opens its OWN window
 * it is a graphical/TTY app (doom, top, forkbomb): it detaches and we return
 * to the prompt immediately, leaving it running in its own window.
 *
 * Priorities are untouched: the child is a normal independent process; this
 * loop only watches it and yields.  stdin is not yet forwarded to the job
 * (CLI tools that read input are a follow-up); other keystrokes are consumed.
 */
static void run_foreground(int pid) {
  /* Extracted to the shared nxexec layer (#193): identical behaviour, one
   * implementation for every terminal-like consumer. */
  nxexec_run_foreground(pid);
}

static void process_command(void) {
  cmd_buf[cmd_len] = '\0';
  if (cmd_len == 0)
    return;

  if (str_eq(cmd_buf, "help") || str_eq(cmd_buf, "?")) {
    print("\n\033[1;33mAvailable Commands:\033[0m\n");
    print("  help            - Show this help\n");
    print("  clear           - Clear screen\n");
    print("  time            - Show uptime\n");
    print("  demo            - Draw 2D shapes\n");
    print("  demo3d          - Launch 3D cube demo\n");
    print("  doom            - Launch doom\n");
    print("  nxshell           - Open new shell window\n");
    print("  ps              - List processes\n");
    print("  ls [path]       - List directory contents\n");
    print("  cd <path>       - Change directory\n");
    print("  pwd             - Show current directory\n");
    print("  cat <path>      - Show file contents\n");
    print("  nxres <w> <h>   - set resolution (also: style|theme|zoom <v>)\n");
    print("  nxproc          - Realtime list processes\n");
    print("  kill <pid>      - Kill process by PID\n");
    print("  nxwins          - List windows (id/pid/state/title)\n");
    print("  nxnotify <msg>  - Send a notification (nxnotify list to read)\n");
    print("  focus <id>      - Focus a window by id (no mouse needed)\n");
    print("  exec <program>  - Execute program (searches /bin, /sys/bin)\n");
    print("  about           - About this OS\n");
    print("  <program>       - Execute program (searches /bin, /sys/bin)\n");
    print("  exit            - Exit shell\n");
  } else if (str_eq(cmd_buf, "clear")) {
    print("\033[2J\033[H");
    shell_redraw();
  } else if (str_eq(cmd_buf, "time")) {
    printf("Uptime: %d seconds (%x ms)\n", (int)(get_time() / 1000),
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
    print("Opening new NXShell...\n");
    int pid = spawn("/sys/bin/nxshell");
    if (pid > 0) {
      printf("Nxshell started. PID=%d\n", pid);
    } else {
      print("Failed to start NXShell\n");
    }
  } else if (str_eq(cmd_buf, "ps")) {
    /* `ps` is delegated to /sys/bin/nxproc (the canonical process-listing
     * ELF, the only place the snapshot/render code lives).  Spawning a
     * dedicated nxproc ELF matches the rest of the `exec`/`demo3d` pattern
     * (shell as dispatcher, real apps as ELFs in /sys/bin).  nxproc also
     * offers a `kill <pid>` sub-command if the user wants to terminate. */
    int pid = spawn("/sys/bin/nxproc");
    if (pid <= 0)
      printf("ps: failed to start nxproc (err %d)\n", pid);
    run_foreground(pid);
  } else if (str_eq(cmd_buf, "ls") ||
             (cmd_buf[0] == 'l' && cmd_buf[1] == 's' && cmd_buf[2] == ' ')) {
    /* NOTE(USR-SHELL-01): Argument parsed with hardcoded byte offsets.
     * "ls" alone uses "."; "ls <path>" takes &cmd_buf[3] as the path. */
    const char *path = ".";
    if (cmd_buf[2] == ' ')
      path = &cmd_buf[3];
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
  } else if (cmd_buf[0] == 'c' && cmd_buf[1] == 'd' &&
             (cmd_buf[2] == ' ' || cmd_buf[2] == '\0')) {
    /* NOTE(USR-SHELL-01): "cd" with no argument defaults to "/"; argument
     * at &cmd_buf[3] (hardcoded offset after "cd "). */
    const char *path = "/";
    if (cmd_buf[2] == ' ')
      path = &cmd_buf[3];
    if (chdir(path) != 0) {
      printf("cd: no such directory: %s\n", path);
    }
  } else if (cmd_buf[0] == 'k' && cmd_buf[1] == 'i' && cmd_buf[2] == 'l' &&
             cmd_buf[3] == 'l' && cmd_buf[4] == ' ') {
    /* Parse PID from "kill <pid>".
     * NOTE(USR-SEC-02): The PID is read directly from user input (decimal
     * digits at cmd_buf[5..]) and passed to kill_process() with no capability
     * check.  Any user can kill any PID including system services (init,
     * notify_srv).  The loop stops at the first non-digit; overflowing int
     * is silent (no range check). */
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
  } else if (cmd_buf[0] == 'f' && cmd_buf[1] == 'o' && cmd_buf[2] == 'c' &&
             cmd_buf[3] == 'u' && cmd_buf[4] == 's' && cmd_buf[5] == ' ') {
    /* focus <window-id>: give a window keyboard focus (reveals it if it was
     * backgrounded) — switch windows without a mouse.  Goes through an
     * OBJ_TYPE_WINDOW capability (OS1_window_focus). */
    int id = 0;
    for (int i = 6; cmd_buf[i] >= '0' && cmd_buf[i] <= '9'; i++)
      id = id * 10 + (cmd_buf[i] - '0');
    if (id > 0) {
      int r = OS1_window_focus(id);
      if (r == 0)
        printf("Focused window %d\n", id);
      else
        printf("focus failed (%d)\n", r);
    } else {
      print("Usage: focus <window-id>\n");
    }
  } else if (str_eq(cmd_buf, "about")) {
    print("\n\033[1;36mNeXs OS v0.0.5.0\033[0m\n");
    print("\033[33mGraphics:\033[0m Window Compositor + ANSI Terminal "
          "Emulator\n");
    print("\033[35mInput:\033[0m Interrupt-driven VirtIO Mouse/Keyboard\n");
    print("\033[32mLibrary:\033[0m POSIX-like userlib with printf support\n");
    print("\nSystem reported: OK\n");
  } else if (str_eq(cmd_buf, "exit")) {
    print("Exiting NXShell...\n");
    running = 0;
    exit(0);
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

    /*
     * exec <program> [args-not-yet-supported]
     *
     * Spawns the named ELF as a new child process, searching /bin/ then
     * /sys/bin/ for relative names (absolute paths bypass the search).
     *
     * Unlike a POSIX exec(3) this does NOT replace the shell process — it
     * uses spawn() which creates a new child; the shell continues running.
     * Renaming this to "run" in a future cleanup would avoid the semantic
     * confusion, but "exec" matches user expectations for "execute this
     * program by name".
     *
     * NOTE(USR-SHELL-01): argument parsed at hardcoded offset cmd_buf[5].
     * NOTE(USR-SEC-03): spawn() grants full ambient authority; no sandbox.
     */
  } else if (cmd_buf[0] == 'e' && cmd_buf[1] == 'x' && cmd_buf[2] == 'e' &&
             cmd_buf[3] == 'c' && cmd_buf[4] == ' ') {
    char *argv[MAX_ARGV];
    int argc = tokenize(&cmd_buf[5], argv, MAX_ARGV);
    if (argc == 0) {
      print("Usage: exec <program> [args...]\n");
    } else {
      char path[SPAWN_PATH_MAX];
      int pid = spawn_search_args(argc, argv, path);
      if (pid > 0) {
        run_foreground(pid); /* in-shell if windowless, else detaches */
      } else {
        printf("exec: not found: %s\n", argv[0]);
      }
    }

  } else {
    /*
     * Unknown command: try to spawn it as an ELF name.
     *
     * spawn_search_args() probes /bin first, then /sys/bin, with argv.
     * Absolute paths (starting with '/') are passed directly to spawn().
     * There is no further PATH search and no shell scripting.
     *
     * NOTE(USR-SEC-03): spawn() grants the new process full ambient authority
     * (arbitrary IPC, kill, registry, spawn); no sandboxing applies.
     */
    char *argv[MAX_ARGV];
    int argc = tokenize(cmd_buf, argv, MAX_ARGV);
    if (argc > 0) {
      char path[SPAWN_PATH_MAX];
      int pid = spawn_search_args(argc, argv, path);
      if (pid > 0) {
        run_foreground(pid); /* in-shell if windowless, else detaches */
      } else {
        printf("Unknown command: %s\n", argv[0]);
      }
    }
  }

  cmd_len = 0;
}

/*
 * main - shell entry point; does not return.
 *
 * 1. Creates a compositor window with position offset by (pid*40)%200 so
 *    multiple shell instances tile without fully overlapping.
 * 2. Calls shell_redraw() and set_focus() to make the window active.
 * 3. Prints the initial prompt (with ANSI colour) to the TTY and mirrors
 *    "shell> " to fd 3 (UART) for serial console visibility.
 * 4. Enters the character-by-character input loop:
 *      - '\n'/'\r' -> process_command(), reprint prompt.
 *      - '\b'/DEL  -> erase last character (ANSI backspace-space-backspace).
 *      - Printable  -> append to cmd_buf, echo to window.
 *    read(0, buf, 1) blocks until a byte is available on the keyboard fd.
 *
 * Side effects: allocates a compositor window, reads from fd 0, writes to
 *   fd 1 (window/TTY), fd 3 (UART mirror), and calls process_command().
 */
int main(void) {
  print("NXShell: Alive\n");
  int pid = get_pid();
  /* Create a unique window for this shell instance.
   * x_off/y_off stagger multiple shell windows by pid so they do not
   * stack exactly on top of each other. */
  char title[32];
  sprintf(title, "NXShell PID %d", pid);

  int x_off = (pid * 40) % 200;
  int y_off = (pid * 40) % 200;
  my_window = create_window(100 + x_off, 100 + y_off, WIN_W, WIN_H, title);

  if (my_window <= 0) {
    print("[NXShell] Error creating window\n");
    exit(1);
  }

  shell_redraw();
  set_focus(get_pid());

  print("\n[NXShell] TTY Window ");
  print_hex(my_window);
  printf(" active (PID %d).\n", get_pid());
  char cwd[128];
  getcwd(cwd, sizeof(cwd));
  printf("\033[32mNXShell\033[0m:\033[34m%s\033[0m> ", cwd);
  write(3, "NXShell> ", 7); /* Mirror to UART */

  /* buf[1] always stays NUL so print(buf) terminates correctly after echoing
   * a single printable character without calling strlen on uninitialized data.
   */
  char buf[2] = {0, 0};
  while (running) {
    long n = read(0, buf, 1); /* Blocking read from keyboard fd */
    if (n <= 0)
      continue;

    char c = buf[0];
    if (c == '\n' || c == '\r') {
      /* End of line: the typed command sits on the prompt line
       * ("NXShell:/>ps"); the user's Enter echoes nothing on its own (CONSOLE
       * read drained the key, didn't echo it), so without a leading newline the
       * dispatched command's output would render immediately after ">ps".  Emit
       * CR+LF first to push the cursor to a fresh line, then dispatch, then
       * reprint the prompt on the next line.  This also keeps the snapshot
       * banner from `ps` (nxproc's inline render) one row below the prompt. */
      print("\r\n");
      process_command();
      if (running) {
        char prompt_cwd[128];
        getcwd(prompt_cwd, sizeof(prompt_cwd));
        /* CR+LF: the compositor's ANSI parser advances on LF, but the terminal
         * cursor stays anchored on the column where the previous output ended
         * unless a CR is also emitted.  Without CR the prompt could still
         * render at column N>0 if the previous output did not end on a clean
         * boundary, which is what `ps` (nxproc's inline render) leaves behind
         * on multi-row tables. */
        print("\r\n");
        printf("\033[32mNXShell\033[0m:\033[34m%s\033[0m> ", prompt_cwd);
      }
    } else if (c == '\b' || c == 127) {
      /* Backspace (0x08) or DEL (0x7F): erase last character.
       * "\b \b" moves back, overwrites with space, moves back again. */
      if (cmd_len > 0) {
        cmd_len--;
        print("\b \b");
      }
    } else if (c >= 32 && c < 127 && cmd_len < 126) {
      /* Printable ASCII: append to buffer and echo to window.
       * Limit is 126 (not 127) to leave room for the NUL terminator. */
      cmd_buf[cmd_len++] = c;
      buf[0] = c;
      buf[1] = 0;
      print(buf);
    }
  }

  exit(0);
  return 0;
}