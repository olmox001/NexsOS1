#include "lua_portability.h"
#include <os1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <locale.h>

/* Time implementation lives in the canonical NexsOS1 libc layer.
 * Lua must use that single ABI surface instead of redefining time(). */

char *tmpnam(char *s) {
  static char static_buf[128];
  static int temp_counter = 0;
  char *buf = s ? s : static_buf;
  /* /home is the ONLY user-writable tree (kernel/fs/vfs.c vfs_write_allowed);
   * the old "/tmpfile_%d" sat at the non-writable root, so os.tmpname()'s path
   * could never be created even once file creation via SYS_FILE_WRITE landed.
   * (fopen("w") still needs open(2) to honour O_CREAT for these to fully work
   * — tracked separately; this at least puts the name in the right tree.) */
  sprintf(buf, "/home/.luatmp_%d", temp_counter++);
  return buf;
}

clock_t clock(void) { return (clock_t)os1_cpu_ns(); }

double difftime(time_t time1, time_t time0) { return (double)(time1 - time0); }

/* strcoll is provided by the central NexsOS libc compatibility layer so that
 * both Coreutils and Lua share the same ABI surface without duplicate symbol
 * definitions at link time. */

/*
 * os1_lua_readline - REPL line reader for nxlua (lua_portability.h's
 * lua_readline override; see that header for why fgets(stdin) is replaced).
 *
 * NOTE(LUA-TTY-01): fgets()/OS1 console read() hand back keyboard.c's BASE
 * ascii_map byte (data1's low byte), not the character after the active
 * keyboard layout's overrides (payload) - the same .key vs .utf8 split
 * input_event_t already exposes for windowed apps. nxlua has no window, so
 * it decodes its own mailbox here instead. Echo is NOT done here: nxexec.h's
 * host-side relay (USR-TTY-01 #123) already echoes every byte it forwards,
 * in the same style as nxshell.c's read(0,...) loop; echoing again here
 * would double every character on screen.
 *
 * Returns 1 with buf NUL-terminated and ending in '\n' (matches fgets(),
 * which pushline() in lua.c strips itself), or 0 on Ctrl-C (EOF-like).
 */
int os1_lua_readline(char *buf, int bufsize) {
  /*
   * NOTE(LUA-TTY-03): the keyboard-mailbox path below is ONLY correct for a
   * real console.  `-i` FORCES interactive mode regardless of
   * lua_stdin_is_tty(), so `lua -i < script` and `echo ... | lua -i` reach the
   * REPL with stdin redirected to a FILE or a PIPE — there the line must come
   * from fd 0, not from the keyboard (otherwise the REPL ignores the supplied
   * program and blocks on a keypress that never comes).  isatty() is the same
   * capability-type test the tty check uses, so the two agree by construction.
   */
  if (!isatty(0))
    return fgets(buf, bufsize, stdin) ? 1 : 0; /* NULL => EOF, like Ctrl-C */

  int len = 0;
  while (1) {
    struct ipc_message m;
    if (try_recv(-1, &m) != 0 || m.type != IPC_TYPE_INPUT || m.data2 == 0) {
      OS1_sleep(15);
      continue;
    }
    char c = m.payload[0];
    if (c == 0x03) /* Ctrl-C: EOF-like */
      return 0;
    if (c == '\r' || c == '\n') {
      if (len + 1 >= bufsize)
        len = bufsize - 2;
      buf[len++] = '\n';
      buf[len] = '\0';
      return 1;
    }
    if (c == '\b' || c == 127) {
      if (len > 0)
        len--;
      continue;
    }
    if (c != 0 && len + 1 < bufsize) {
      buf[len++] = c;
      buf[len] = '\0';
    }
  }
}