/*
 * user/sys/bin/regedit.c
 * Registry Editor (Control Panel)
 *
 * Windowed, read-only registry viewer: enumerates every key in the global
 * registry (OS1_registry_enum) and renders "key = value" rows through the
 * window terminal emulator (ANSI + printf_win), refreshing only when the
 * registry content actually changes — same render-if-changed pattern as
 * nxproc.h/nxtop.  Editing keys is future work (needs an input line editor).
 *
 * Reads are intentionally ungated kernel-side; writes elsewhere are gated by
 * CAP_REG_WRITE (kernel/lib/registry.c) — this tool never writes.
 *
 * Known issues:
 *   USR-BLOAT-01/02 (W2 BAD-IMPL·PERF) ~500KB ELF due to unconditional
 *                  stb_image/stb_easy_font in lib.o and retained DWARF debug.
 */
#include <os1.h>

#define ENUM_BUF_SZ 2048 /* newline-separated key list from the kernel */
#define VAL_BUF_SZ 128   /* single value buffer */

/* Tiny FNV-1a over the rendered content, to skip unchanged redraws. */
static unsigned long regedit_hash(unsigned long h, const char *s) {
  while (*s) {
    h ^= (unsigned char)*s++;
    h *= 1099511628211UL;
  }
  return h;
}

/*
 * render - clear the window and print one "key = value" row per registry key.
 * Returns the content hash so the caller can skip identical frames.
 */
static unsigned long render(int win_id, int do_write) {
  char keys[ENUM_BUF_SZ];
  char val[VAL_BUF_SZ];
  unsigned long h = 1469598103934665603UL;

  int n = OS1_registry_enum(keys, sizeof(keys));
  if (n < 0) {
    if (do_write)
      _sys_window_write(win_id, "\033[H\033[Jregistry enum failed\n", 27);
    return ~0UL;
  }

  /* First pass just hashes; the caller re-invokes with do_write=1 on change. */
  if (do_write) {
    _sys_window_write(win_id, "\033[H\033[J", 6);
    _sys_window_write(win_id, "\033[1;34m", 7);
    printf_win(win_id, "%-24s %s\n", "KEY", "VALUE");
    _sys_window_write(win_id, "\033[0m", 4);
    _sys_window_write(win_id, "----------------------------------------\n", 42);
  }

  char *p = keys;
  while (*p) {
    char *nl = p;
    while (*nl && *nl != '\n')
      nl++;
    int last = (*nl == '\0');
    *nl = '\0';

    if (*p) {
      val[0] = '\0';
      if (OS1_registry_get(p, val, sizeof(val)) != 0)
        val[0] = '\0'; /* directory node or vanished key: show empty */
      h = regedit_hash(h, p);
      h = regedit_hash(h, val);
      if (do_write)
        printf_win(win_id, "%-24s %s\n", p, val);
    }

    if (last)
      break;
    p = nl + 1;
  }
  return h;
}

int main(void) {
  int win_id = create_window(100, 100, 400, 300, "Control Panel");
  if (win_id < 0)
    return 1;

  unsigned long last = 0; /* impossible hash: guarantees the first render */
  while (1) {
    unsigned long sig = render(win_id, 0);
    if (sig != last) {
      render(win_id, 1);
      last = sig;
    }
    OS1_sleep(1000);
  }

  return 0;
}
