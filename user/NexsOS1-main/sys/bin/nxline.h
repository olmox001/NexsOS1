#ifndef _USER_NXLINE_H
#define _USER_NXLINE_H

/*
 * user/sys/bin/nxline.h
 * NEXS interactive line editor (ASTRA service layer) — cursor movement,
 * persistent history, incremental search, and command/path completion for
 * a single input line hosted in a compositor terminal window.
 *
 * Header-only, static-inline, same pattern as nxexec.h/nxassoc.h/nxinfo.h.
 * Extracted out of nxshell.c's original "cmd_buf + cmd_len, backspace-only"
 * editor so any future interactive prompt (a REPL, a debugger console) can
 * reuse the same editing model instead of re-deriving it.
 *
 * RENDERING MODEL: nxline never touches window pixels or the cell grid
 * directly. It drives the SAME VT100/ANSI terminal emulator every other
 * program already writes through (kernel/graphics/term.c: term_write /
 * handle_csi), via plain printf()/print() escape sequences:
 *
 *   \b              backspace (cursor left one cell — term.c handles it)
 *   \033[C  \033[D   cursor right/left by N cells (final 'C'/'D', handle_csi)
 *   \033[K           erase from cursor to end of line (final 'K', mode 0)
 *
 * Every edit below is expressed as a RELATIVE cursor movement from wherever
 * the cursor already is, never an absolute column. That means nxline needs
 * no knowledge of the visible width of a colored, theme-dependent prompt
 * string (nxshell's prompt embeds \033[32m/\033[34m SGR codes whose glyphs
 * take zero columns) — it only ever moves relative to its own last output.
 *
 * INPUT MODEL: nxshell already receives ASCII + scancode pairs via
 * input_poll_event() (INPUT_TYPE_KEYBOARD). Control-letter combinations
 * arrive as ASCII control codes (nxexec.h already treats 0x03 as Ctrl-C on
 * this same path) — nxline follows that precedent: Ctrl-A/E/L/R/D are
 * 0x01/0x05/0x0C/0x12/0x04 (the letter's position in the alphabet, exactly
 * like the existing Ctrl-C=0x03). Arrow keys / Home / End / Delete have no
 * ASCII representation and are read from ev.keyboard.scancode instead, via
 * INPUT_KEY_LEFT/RIGHT/UP/DOWN/HOME/END/DELETE — ASSUMED to sit in input.h
 * alongside the already-used INPUT_KEY_ENTER/INPUT_KEY_BACKSPACE. If they
 * are not yet defined there, add them to the keyboard driver's scancode
 * table before arrow-key/Home/End/Delete input will do anything here;
 * everything else (Ctrl combos, history, completion, plain typing) works
 * without them.
 *
 * HISTORY PERSISTENCE: the whole in-memory ring is serialized newline-
 * separated and written with OS1_fs_write(path, buf, n, 0) on every new
 * entry. offset 0 + ext4_write's truncate-to-length behaviour (see lib.c
 * rename()'s comment) means this is always a clean overwrite, never stale
 * trailing garbage from a longer previous file. Bounded to
 * NXLINE_HIST_MAX * NXLINE_MAX bytes (16 KB at the defaults below), so a
 * full rewrite per command is cheap enough not to need incremental append.
 */

#include <os1.h>
#include <input.h>
#include <string.h>

/* nxline_contains - tiny local substring search, used only by Ctrl-R
 * incremental search below. memmove/strlen/strncmp/strchr are confirmed
 * present in this libc (memmove is already used in term.c; the others are
 * used throughout nxexec.h/nxassoc.h), but strstr() appears nowhere in the
 * existing codebase — so this avoids assuming it exists rather than risk a
 * link failure over one call site. */
static inline int nxline_contains(const char *hay, const char *needle) {
  if (!*needle)
    return 1;
  size_t nl = strlen(needle);
  for (const char *p = hay; *p; p++)
    if (strncmp(p, needle, nl) == 0)
      return 1;
  return 0;
}

#define NXLINE_MAX 256
#define NXLINE_HIST_MAX 64

enum {
  NXLINE_NONE = 0,        /* keep editing */
  NXLINE_SUBMIT,          /* Enter: nl->buf/nl->len hold the finished line */
  NXLINE_EOF,             /* Ctrl-D on an empty line */
  NXLINE_CLEAR_SCREEN,    /* Ctrl-L: caller should clear the screen and repaint */
};

struct nxline {
  char buf[NXLINE_MAX];
  int len;    /* bytes currently in buf (not counting the NUL) */
  int cursor; /* 0..len, insertion point */

  char history[NXLINE_HIST_MAX][NXLINE_MAX];
  int hist_count; /* entries currently stored (<= NXLINE_HIST_MAX) */
  int hist_pos;   /* -1 = editing the live line; else index into history[] */
  char live_stash[NXLINE_MAX]; /* live buffer, saved when history browsing starts */

  int searching; /* 1 while a Ctrl-R incremental search is active */
  char search_buf[NXLINE_MAX];
  int search_len;
  char search_stash[NXLINE_MAX]; /* buf contents when Ctrl-R was pressed */

  char hist_path[64]; /* e.g. "/home/.nxshell_history"; empty = no persistence */

  /* Caller hook: print JUST the prompt string (no leading newline, no line
   * content) — invoked by nxline when it needs to repaint a whole line from
   * column 0 (Ctrl-L, multi-match tab listing). ctx is passed through
   * unchanged (nxshell has no per-instance state today, but this keeps the
   * header reusable for a future multi-window consumer). */
  void (*print_prompt)(void *ctx);
  void *ctx;
};

/* ---------------- init / history load-save ---------------- */

static inline void nxline_init(struct nxline *nl, const char *hist_path,
                               void (*print_prompt)(void *), void *ctx) {
  memset(nl, 0, sizeof(*nl));
  nl->hist_pos = -1;
  nl->print_prompt = print_prompt;
  nl->ctx = ctx;
  if (hist_path)
    snprintf(nl->hist_path, sizeof(nl->hist_path), "%s", hist_path);
}

/* nxline_load_history - populate history[] from hist_path, if set. Silent
 * no-op if the file doesn't exist yet (first run) or persistence is off. */
static inline void nxline_load_history(struct nxline *nl) {
  if (!nl->hist_path[0])
    return;
  char buf[NXLINE_HIST_MAX * NXLINE_MAX];
  int n = file_read(nl->hist_path, buf, sizeof(buf) - 1, 0);
  if (n <= 0)
    return;
  buf[n] = '\0';

  nl->hist_count = 0;
  char *save = NULL;
  for (char *line = strtok_r(buf, "\n", &save); line && nl->hist_count < NXLINE_HIST_MAX;
       line = strtok_r(NULL, "\n", &save)) {
    if (!*line)
      continue;
    snprintf(nl->history[nl->hist_count], NXLINE_MAX, "%s", line);
    nl->hist_count++;
  }
}

/* nxline_save_history - flush the in-memory ring back to hist_path (a full
 * overwrite; see the header comment on why that's safe and cheap here). */
static inline void nxline_save_history(struct nxline *nl) {
  if (!nl->hist_path[0])
    return;
  char buf[NXLINE_HIST_MAX * NXLINE_MAX];
  int n = 0;
  for (int i = 0; i < nl->hist_count; i++) {
    int l = (int)strlen(nl->history[i]);
    if (n + l + 1 >= (int)sizeof(buf))
      break;
    memcpy(buf + n, nl->history[i], l);
    n += l;
    buf[n++] = '\n';
  }
  OS1_fs_write(nl->hist_path, buf, n, 0);
}

/* nxline_history_add - push a finished command onto the ring (dropping the
 * oldest entry once full) and persist it. Skips blank lines and an exact
 * repeat of the immediately-previous entry (the common readline behaviour:
 * mashing Enter or repeating the same command doesn't spam history). */
static inline void nxline_history_add(struct nxline *nl, const char *cmd) {
  if (!cmd || !*cmd)
    return;
  if (nl->hist_count > 0 &&
      strncmp(nl->history[nl->hist_count - 1], cmd, NXLINE_MAX) == 0)
    return;

  if (nl->hist_count < NXLINE_HIST_MAX) {
    snprintf(nl->history[nl->hist_count], NXLINE_MAX, "%s", cmd);
    nl->hist_count++;
  } else {
    /* ring: drop oldest, shift everyone down one */
    for (int i = 1; i < NXLINE_HIST_MAX; i++)
      memcpy(nl->history[i - 1], nl->history[i], NXLINE_MAX);
    snprintf(nl->history[NXLINE_HIST_MAX - 1], NXLINE_MAX, "%s", cmd);
  }
  nxline_save_history(nl);
}

/* ---------------- low-level relative-cursor primitives ---------------- */

static inline void nxline_move_left_n(int n) {
  if (n > 0)
    printf("\033[%dD", n);
}
static inline void nxline_move_right_n(int n) {
  if (n > 0)
    printf("\033[%dC", n);
}

/* nxline_replace_line - wipe the currently-displayed line content (NOT the
 * prompt) and print newtext in its place. Used by history browsing and
 * Ctrl-R acceptance. Purely relative: moves left by the CURRENT cursor
 * position to reach column 0 of the input, erases to end of line, prints
 * newtext, cursor lands at the end. */
static inline void nxline_replace_line(struct nxline *nl, const char *newtext) {
  nxline_move_left_n(nl->cursor);
  print("\033[K");
  size_t tl = strlen(newtext);
  if (tl >= NXLINE_MAX)
    tl = NXLINE_MAX - 1;
  memcpy(nl->buf, newtext, tl);
  nl->buf[tl] = '\0';
  nl->len = (int)tl;
  nl->cursor = (int)tl;
  print(nl->buf);
}

/* nxline_repaint_inline - reprint the CURRENT buffer starting at column 0,
 * assuming the caller already positioned the cursor there (e.g. right after
 * printing a fresh prompt on a new line). Leaves the terminal cursor at
 * nl->cursor, not necessarily at the end. */
static inline void nxline_repaint_inline(struct nxline *nl) {
  print(nl->buf);
  if (nl->cursor < nl->len)
    nxline_move_left_n(nl->len - nl->cursor);
}

/* ---------------- editing operations ---------------- */

static inline void nxline_insert_char(struct nxline *nl, char c) {
  if (nl->len >= NXLINE_MAX - 1)
    return;
  memmove(&nl->buf[nl->cursor + 1], &nl->buf[nl->cursor], nl->len - nl->cursor);
  nl->buf[nl->cursor] = c;
  nl->len++;
  nl->buf[nl->len] = '\0';
  print(&nl->buf[nl->cursor]); /* prints the new char + the shifted tail */
  nl->cursor++;
  nxline_move_left_n(nl->len - nl->cursor); /* put the caret back after c */
}

static inline void nxline_delete_before(struct nxline *nl) { /* Backspace */
  if (nl->cursor == 0)
    return;
  memmove(&nl->buf[nl->cursor - 1], &nl->buf[nl->cursor], nl->len - nl->cursor);
  nl->cursor--;
  nl->len--;
  nl->buf[nl->len] = '\0';
  print("\b");
  print(&nl->buf[nl->cursor]);
  print(" "); /* erase the leftover glyph from the old, longer line */
  nxline_move_left_n(nl->len - nl->cursor + 1);
}

static inline void nxline_delete_at(struct nxline *nl) { /* Delete (forward) */
  if (nl->cursor >= nl->len)
    return;
  memmove(&nl->buf[nl->cursor], &nl->buf[nl->cursor + 1],
          nl->len - nl->cursor - 1);
  nl->len--;
  nl->buf[nl->len] = '\0';
  print(&nl->buf[nl->cursor]);
  print(" ");
  nxline_move_left_n(nl->len - nl->cursor + 1);
}

static inline void nxline_move_left(struct nxline *nl) {
  if (nl->cursor > 0) {
    print("\033[D");
    nl->cursor--;
  }
}
static inline void nxline_move_right(struct nxline *nl) {
  if (nl->cursor < nl->len) {
    print("\033[C");
    nl->cursor++;
  }
}
static inline void nxline_move_home(struct nxline *nl) {
  nxline_move_left_n(nl->cursor);
  nl->cursor = 0;
}
static inline void nxline_move_end(struct nxline *nl) {
  nxline_move_right_n(nl->len - nl->cursor);
  nl->cursor = nl->len;
}

/* ---------------- history browsing ---------------- */

static inline void nxline_history_prev(struct nxline *nl) {
  if (nl->hist_count == 0)
    return;
  if (nl->hist_pos == -1) {
    snprintf(nl->live_stash, NXLINE_MAX, "%s", nl->buf);
    nl->hist_pos = nl->hist_count - 1;
  } else if (nl->hist_pos > 0) {
    nl->hist_pos--;
  } else {
    return; /* already at the oldest entry */
  }
  nxline_replace_line(nl, nl->history[nl->hist_pos]);
}

static inline void nxline_history_next(struct nxline *nl) {
  if (nl->hist_pos == -1)
    return;
  nl->hist_pos++;
  if (nl->hist_pos >= nl->hist_count) {
    nl->hist_pos = -1;
    nxline_replace_line(nl, nl->live_stash);
  } else {
    nxline_replace_line(nl, nl->history[nl->hist_pos]);
  }
}

/* ---------------- Ctrl-R incremental search ---------------- */

/* nxline_search_render - redraw as "(reverse-i-search)`query': match". This
 * temporarily overwrites the visible line the same way history browsing
 * does (relative erase + reprint); the ACTUAL nl->buf is untouched until
 * the search is accepted (Enter) or cancelled (Esc/Ctrl-G). */
static inline void nxline_search_render(struct nxline *nl) {
  nxline_move_left_n(nl->cursor);
  print("\033[K");
  char line[NXLINE_MAX];
  const char *match = "";
  for (int i = nl->hist_count - 1; i >= 0; i--) {
    if (nl->search_len == 0 || nxline_contains(nl->history[i], nl->search_buf)) {
      match = nl->history[i];
      break;
    }
  }
  snprintf(line, sizeof(line), "(reverse-i-search)`%s': %s", nl->search_buf,
           match);
  print(line);
  /* cursor tracking during search is a display-only concept: park it at the
   * end of the rendered line (cursor math resumes once the search ends). */
  nl->cursor = (int)strlen(line);
  nl->len = nl->cursor;
  /* stash the actual match so Enter can accept it without re-searching */
  snprintf(nl->buf, NXLINE_MAX, "%s", match);
}

static inline void nxline_search_start(struct nxline *nl) {
  nl->searching = 1;
  nl->search_len = 0;
  nl->search_buf[0] = '\0';
  snprintf(nl->search_stash, NXLINE_MAX, "%s", nl->buf);
  nxline_search_render(nl);
}

static inline void nxline_search_cancel(struct nxline *nl) {
  nl->searching = 0;
  nxline_replace_line(nl, nl->search_stash);
}

static inline void nxline_search_accept(struct nxline *nl) {
  /* nl->buf already holds the matched command (set by nxline_search_render);
   * just stop searching and leave it as the live line, cursor at end. */
  nl->searching = 0;
  nl->len = (int)strlen(nl->buf);
  nl->cursor = nl->len;
  nxline_replace_line(nl, nl->buf); /* re-render cleanly without the search prefix */
}

/* ---------------- tab completion ---------------- */

/* nxline_word_start - index of the start of the word ending at nl->cursor
 * (whitespace-delimited), for completion. */
static inline int nxline_word_start(struct nxline *nl) {
  int i = nl->cursor;
  while (i > 0 && nl->buf[i - 1] != ' ' && nl->buf[i - 1] != '\t')
    i--;
  return i;
}

/* nxline_complete - Tab handling. Completes either a bare command name
 * (scanning /bin then /sys/bin, mirroring nxshell's own help_list_programs)
 * or, if the current word contains '/', a path (scanning that directory).
 * A single match is inserted inline; multiple matches are listed on a new
 * line and the prompt+buffer are repainted below them via print_prompt(). */
static inline void nxline_complete(struct nxline *nl) {
  int ws = nxline_word_start(nl);
  char word[NXLINE_MAX];
  int wl = nl->cursor - ws;
  if (wl <= 0 || wl >= NXLINE_MAX)
    return;
  memcpy(word, &nl->buf[ws], wl);
  word[wl] = '\0';

  const char *slash = strrchr(word, '/');
  char dir[128];
  const char *prefix;
  if (slash) {
    int dl = (int)(slash - word);
    if (dl == 0)
      snprintf(dir, sizeof(dir), "/");
    else
      snprintf(dir, sizeof(dir), "%.*s", dl, word);
    prefix = slash + 1;
  } else {
    dir[0] = '\0'; /* sentinel: search /bin then /sys/bin below */
    prefix = word;
  }
  size_t plen = strlen(prefix);

  char matches[16][NXLINE_MAX];
  int nmatch = 0;
  char listing[2][1024];
  int nlisting = dir[0] ? 1 : 2;
  if (dir[0]) {
    int n = list_dir(dir, listing[0], sizeof(listing[0]) - 1);
    listing[0][n > 0 ? n : 0] = '\0';
  } else {
    int n0 = list_dir("/bin", listing[0], sizeof(listing[0]) - 1);
    listing[0][n0 > 0 ? n0 : 0] = '\0';
    int n1 = list_dir("/sys/bin", listing[1], sizeof(listing[1]) - 1);
    listing[1][n1 > 0 ? n1 : 0] = '\0';
  }

  for (int li = 0; li < nlisting && nmatch < 16; li++) {
    char *save = NULL;
    for (char *tok = strtok_r(listing[li], " \t", &save); tok && nmatch < 16;
         tok = strtok_r(NULL, " \t", &save)) {
      if (plen == 0 || strncmp(tok, prefix, plen) == 0) {
        int already = 0;
        for (int i = 0; i < nmatch; i++)
          if (strcmp(matches[i], tok) == 0)
            already = 1;
        if (!already)
          snprintf(matches[nmatch++], NXLINE_MAX, "%s", tok);
      }
    }
  }

  if (nmatch == 0)
    return;

  if (nmatch == 1) {
    const char *suffix = matches[0] + plen;
    while (*suffix)
      nxline_insert_char(nl, *suffix++);
    return;
  }

  /* Multiple matches: list them, then repaint prompt + current buffer. */
  print("\r\n");
  for (int i = 0; i < nmatch; i++)
    printf("%-16s", matches[i]);
  print("\r\n");
  if (nl->print_prompt)
    nl->print_prompt(nl->ctx);
  nxline_repaint_inline(nl);
}

/* ---------------- main key-feed entry point ---------------- */

/* nxline_feed_key - handle one keyboard event. Returns an NXLINE_* code;
 * NXLINE_SUBMIT means nl->buf (NUL-terminated, nl->len bytes) is the
 * completed line — the caller should process it, then call
 * nxline_history_add() and reset nl->len/cursor to 0 for the next line. */
static inline int nxline_feed_key(struct nxline *nl, unsigned char key,
                                  uint16_t scancode) {
  if (nl->searching) {
    if (key == '\n' || key == '\r' || scancode == INPUT_KEY_ENTER) {
      nxline_search_accept(nl);
      return NXLINE_NONE;
    }
    if (key == 0x07 || key == 0x1B) { /* Ctrl-G / Esc: cancel */
      nxline_search_cancel(nl);
      return NXLINE_NONE;
    }
    if (key == 0x12) { /* Ctrl-R again: skip to the next older match */
      return NXLINE_NONE; /* simplest correct behaviour: no-op, avoids
                            * accidentally cycling past the only match */
    }
    if ((key == '\b' || key == 127) && nl->search_len > 0) {
      nl->search_buf[--nl->search_len] = '\0';
      nxline_search_render(nl);
      return NXLINE_NONE;
    }
    if (key >= 32 && key < 127 && nl->search_len < NXLINE_MAX - 1) {
      nl->search_buf[nl->search_len++] = (char)key;
      nl->search_buf[nl->search_len] = '\0';
      nxline_search_render(nl);
    }
    return NXLINE_NONE;
  }

  if (key == '\n' || key == '\r' || scancode == INPUT_KEY_ENTER)
    return NXLINE_SUBMIT;

  if (key == '\b' || key == 127 || scancode == INPUT_KEY_BACKSPACE) {
    nxline_delete_before(nl);
    return NXLINE_NONE;
  }

  if (key == 0x01) { /* Ctrl-A */
    nxline_move_home(nl);
    return NXLINE_NONE;
  }
  if (key == 0x05) { /* Ctrl-E */
    nxline_move_end(nl);
    return NXLINE_NONE;
  }
  if (key == 0x0C) /* Ctrl-L */
    return NXLINE_CLEAR_SCREEN;
  if (key == 0x12) { /* Ctrl-R */
    nxline_search_start(nl);
    return NXLINE_NONE;
  }
  if (key == 0x04) { /* Ctrl-D */
    if (nl->len == 0)
      return NXLINE_EOF;
    nxline_delete_at(nl); /* non-empty line: acts as forward-delete */
    return NXLINE_NONE;
  }
  if (key == '\t') {
    nxline_complete(nl);
    return NXLINE_NONE;
  }

  if (scancode == INPUT_KEY_LEFT) {
    nxline_move_left(nl);
    return NXLINE_NONE;
  }
  if (scancode == INPUT_KEY_RIGHT) {
    nxline_move_right(nl);
    return NXLINE_NONE;
  }
  if (scancode == INPUT_KEY_UP) {
    nxline_history_prev(nl);
    return NXLINE_NONE;
  }
  if (scancode == INPUT_KEY_DOWN) {
    nxline_history_next(nl);
    return NXLINE_NONE;
  }
  if (scancode == INPUT_KEY_HOME) {
    nxline_move_home(nl);
    return NXLINE_NONE;
  }
  if (scancode == INPUT_KEY_END) {
    nxline_move_end(nl);
    return NXLINE_NONE;
  }
  if (scancode == INPUT_KEY_DELETE) {
    nxline_delete_at(nl);
    return NXLINE_NONE;
  }

  if (key >= 32 && key < 127)
    nxline_insert_char(nl, (char)key);

  return NXLINE_NONE;
}

/* nxline_reset - clear the buffer for the next line (call after SUBMIT is
 * handled and, typically, nxline_history_add() has been called). */
static inline void nxline_reset(struct nxline *nl) {
  nl->len = 0;
  nl->cursor = 0;
  nl->buf[0] = '\0';
  nl->hist_pos = -1;
}

#endif /* _USER_NXLINE_H */
