/*
 * libctest — conformance harness for the stream layer and the two layers under
 * it.
 *
 * WHY THIS EXISTS.  Every stdio defect this system has hit was found by an
 * APPLICATION, not by a test: doom's savegame load (unbuffered reads against
 * ext4's 4 KiB block granularity), the file manager's "cannot open directory"
 * (two consumers relying on an unwritten invariant), and Lua failing to load a
 * script with a `#!` line (fgetc bypassing the read buffer, so a later fread
 * refilled from a stale offset).  Each was a pattern some real program used and
 * no test did.  So the cases here are named after the program whose behaviour
 * they encode — when one fails, the failure names the app that will break.
 *
 * LAYERING.  POSIX and the C library are a personality ABOVE the native OS1
 * layer; the native layer must not depend on them.  This harness therefore
 * exercises both and, in group C, asserts they AGREE — a divergence between
 * `stat()` and what a stream reports is the shape the file-manager bug had.
 *
 * Output matches captest: one line per case, then a machine-greppable summary.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <os1.h>
#include <execsvc.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass, g_fail;

static void ok(const char *name) {
  g_pass++;
  printf("[libctest] PASS %s\n", name);
}

static void bad(const char *name, const char *why) {
  g_fail++;
  printf("[libctest] FAIL %s: %s\n", name, why);
}

#define CHECK(name, cond, why)                                                 \
  do {                                                                         \
    if (!(cond)) {                                                             \
      bad((name), (why));                                                      \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define TMP "/home/.libctest.tmp"

/* write_file - lay down exact bytes with the NATIVE layer, so a stream defect
 * can never fabricate the fixture it is then measured against. */
static int write_file(const char *path, const char *data, int len) {
  return OS1_fs_write(path, data, len, 0) == len ? 0 : -1;
}

/* ---------- group A: the stream layer, per real application pattern ------- */

/*
 * A1 — Lua's loader.  lauxlib.c skipcomment() consumes a `#!` line with getc()
 * and leaves the FIRST byte of line 2 in hand; getF() then switches to fread()
 * for the rest.  The two must be one continuous stream.  When fgetc bypassed
 * the read buffer it also never advanced fp->pos, so fread refilled from 0 and
 * re-read the file from the top — Lua saw "-" followed by "#" and reported
 * "unexpected symbol near '-'".
 */
static void t_lua_shebang_getc_then_fread(void) {
  const char *name = "lua/getc-then-fread";
  const char *src = "#!../lua\n-- comment\nprint(1)\n";
  int len = (int)strlen(src);
  CHECK(name, write_file(TMP, src, len) == 0, "fixture write failed");

  FILE *f = fopen(TMP, "r");
  CHECK(name, f != NULL, "fopen failed");

  int c = fgetc(f); /* skipBOM */
  while (c != EOF && c != '\n')
    c = fgetc(f); /* skip the shebang line */
  c = fgetc(f);   /* first byte of line 2 */
  CHECK(name, c == '-', "getc did not land on line 2");

  char rest[64];
  size_t n = fread(rest, 1, sizeof(rest), f);
  fclose(f);
  CHECK(name, n == (size_t)len - 10, "fread returned the wrong length");
  CHECK(name, memcmp(rest, src + 10, n) == 0,
        "fread did not continue where getc stopped");
  ok(name);
}

/* A2 — the reverse order: a block read, then byte reads. */
static void t_fread_then_getc(void) {
  const char *name = "stream/fread-then-getc";
  const char *src = "ABCDEFGHIJ";
  CHECK(name, write_file(TMP, src, 10) == 0, "fixture write failed");
  FILE *f = fopen(TMP, "r");
  CHECK(name, f != NULL, "fopen failed");
  char head[4];
  CHECK(name, fread(head, 1, 4, f) == 4, "short fread");
  CHECK(name, memcmp(head, "ABCD", 4) == 0, "wrong head");
  CHECK(name, fgetc(f) == 'E', "getc did not resume after fread");
  CHECK(name, fgetc(f) == 'F', "getc did not advance");
  fclose(f);
  ok(name);
}

/*
 * A3 — fputc/fputs must share the write buffer with fwrite.  While they wrote
 * straight to the descriptor, buffered bytes flushed LATER and landed at a
 * different offset: the file came out reordered, silently.
 */
static void t_fputc_fputs_fwrite_ordering(void) {
  const char *name = "stream/fputc-fputs-fwrite-order";
  OS1_fs_unlink(TMP);
  FILE *f = fopen(TMP, "w");
  CHECK(name, f != NULL, "fopen(w) failed");
  fputc('A', f);
  fputs("BC", f);
  fwrite("DE", 1, 2, f);
  fputc('F', f);
  CHECK(name, fclose(f) == 0, "fclose failed");

  char back[16];
  int r = OS1_fs_read(TMP, back, sizeof(back), 0);
  CHECK(name, r == 6, "file length wrong");
  CHECK(name, memcmp(back, "ABCDEF", 6) == 0, "bytes landed out of order");
  ok(name);
}

/*
 * A4 — read-after-write and write-after-read on ONE stream.  fread flushes
 * pending writes; fwrite drops the cached read window.  Without the second
 * half, a read following a write is served stale bytes from the old position.
 */
static void t_interleaved_read_write(void) {
  const char *name = "stream/interleaved-rw";
  CHECK(name, write_file(TMP, "0123456789", 10) == 0, "fixture write failed");
  FILE *f = fopen(TMP, "r+");
  CHECK(name, f != NULL, "fopen(r+) failed");
  char c4[4];
  CHECK(name, fread(c4, 1, 4, f) == 4, "short fread");
  CHECK(name, memcmp(c4, "0123", 4) == 0, "wrong prefix");
  CHECK(name, fwrite("XY", 1, 2, f) == 2, "short fwrite");
  char c2[2];
  CHECK(name, fread(c2, 1, 2, f) == 2, "short fread after write");
  CHECK(name, memcmp(c2, "67", 2) == 0, "stale read window after write");
  fclose(f);
  char back[16];
  CHECK(name, OS1_fs_read(TMP, back, sizeof(back), 0) == 10, "length changed");
  CHECK(name, memcmp(back, "0123XY6789", 10) == 0, "write landed wrong");
  ok(name);
}

/* A5 — POSIX: a successful fseek undoes the effects of ungetc. */
static void t_fseek_undoes_ungetc(void) {
  const char *name = "posix/fseek-undoes-ungetc";
  CHECK(name, write_file(TMP, "abcdef", 6) == 0, "fixture write failed");
  FILE *f = fopen(TMP, "r");
  CHECK(name, f != NULL, "fopen failed");
  CHECK(name, fgetc(f) == 'a', "first byte wrong");
  CHECK(name, ungetc('Z', f) == 'Z', "ungetc rejected");
  CHECK(name, fseek(f, 3, SEEK_SET) == 0, "fseek failed");
  CHECK(name, fgetc(f) == 'd', "pushback survived the seek");
  fclose(f);
  ok(name);
}

/* A6 — ungetc is visible to fread, not only to fgetc. */
static void t_ungetc_then_fread(void) {
  const char *name = "posix/ungetc-then-fread";
  CHECK(name, write_file(TMP, "abcdef", 6) == 0, "fixture write failed");
  FILE *f = fopen(TMP, "r");
  CHECK(name, f != NULL, "fopen failed");
  CHECK(name, fgetc(f) == 'a', "first byte wrong");
  CHECK(name, ungetc('a', f) == 'a', "ungetc rejected");
  char b[3];
  CHECK(name, fread(b, 1, 3, f) == 3, "short fread");
  CHECK(name, memcmp(b, "abc", 3) == 0, "fread ignored the pushback");
  fclose(f);
  ok(name);
}

/*
 * A7 — a line straddling the 4096-byte buffer boundary.  kilo reads files line
 * by line; a refill in the middle of a line is exactly where an off-by-one in
 * the buffer accounting shows up.
 */
static void t_fgets_across_buffer_boundary(void) {
  const char *name = "kilo/fgets-across-4k-boundary";
  static char big[FILE_RBUF_SIZE + 200];
  int n = 0;
  while (n < FILE_RBUF_SIZE - 4)
    big[n++] = 'x';
  big[n++] = '\n';
  const char *tail = "straddle-me-across-the-refill\n";
  memcpy(big + n, tail, strlen(tail));
  n += (int)strlen(tail);
  CHECK(name, write_file(TMP, big, n) == 0, "fixture write failed");

  FILE *f = fopen(TMP, "r");
  CHECK(name, f != NULL, "fopen failed");
  static char line[FILE_RBUF_SIZE + 200];
  CHECK(name, fgets(line, sizeof(line), f) != NULL, "first fgets failed");
  CHECK(name, (int)strlen(line) == FILE_RBUF_SIZE - 3, "first line truncated");
  CHECK(name, fgets(line, sizeof(line), f) != NULL, "second fgets failed");
  CHECK(name, strcmp(line, tail) == 0, "line lost across the buffer refill");
  fclose(f);
  ok(name);
}

/* A8 — ftell agrees with the position every path thinks it is at. */
static void t_ftell_tracks_every_path(void) {
  const char *name = "stream/ftell-tracks-all-paths";
  CHECK(name, write_file(TMP, "0123456789", 10) == 0, "fixture write failed");
  FILE *f = fopen(TMP, "r");
  CHECK(name, f != NULL, "fopen failed");
  CHECK(name, ftell(f) == 0, "initial ftell not 0");
  fgetc(f);
  CHECK(name, ftell(f) == 1, "ftell did not follow fgetc");
  char b[4];
  fread(b, 1, 4, f);
  CHECK(name, ftell(f) == 5, "ftell did not follow fread");
  fseek(f, 2, SEEK_CUR);
  CHECK(name, ftell(f) == 7, "ftell did not follow SEEK_CUR");
  fseek(f, 0, SEEK_END);
  CHECK(name, ftell(f) == 10, "SEEK_END did not reach the size");
  fclose(f);
  ok(name);
}

/* A9 — EOF is set by reading PAST the end, not by reaching it; rewind clears
 * it again (and the error indicator, which plain fseek does not). */
static void t_eof_and_rewind(void) {
  const char *name = "posix/eof-and-rewind";
  CHECK(name, write_file(TMP, "abc", 3) == 0, "fixture write failed");
  FILE *f = fopen(TMP, "r");
  CHECK(name, f != NULL, "fopen failed");
  char b[3];
  CHECK(name, fread(b, 1, 3, f) == 3, "short fread");
  CHECK(name, fgetc(f) == EOF, "read past end did not report EOF");
  CHECK(name, feof(f) != 0, "feof not set after reading past the end");
  rewind(f);
  CHECK(name, feof(f) == 0, "rewind did not clear EOF");
  CHECK(name, ftell(f) == 0, "rewind did not reset the position");
  CHECK(name, fgetc(f) == 'a', "rewind did not restore readability");
  fclose(f);
  ok(name);
}

/* A10 — fread returns COMPLETE elements, not bytes. */
static void t_fread_partial_element(void) {
  const char *name = "posix/fread-complete-elements";
  CHECK(name, write_file(TMP, "1234567", 7) == 0, "fixture write failed");
  FILE *f = fopen(TMP, "r");
  CHECK(name, f != NULL, "fopen failed");
  char b[16];
  size_t got = fread(b, 3, 5, f); /* 7 bytes available => 2 whole elements */
  CHECK(name, got == 2, "did not count whole elements");
  fclose(f);
  ok(name);
}

/* A11 — doom's savegame shape: many small writes, reopened and read back in
 * many small reads.  This is the case whose unbuffered form took minutes. */
static void t_doom_savegame_roundtrip(void) {
  const char *name = "doom/savegame-roundtrip";
  OS1_fs_unlink(TMP);
  FILE *w = fopen(TMP, "w");
  CHECK(name, w != NULL, "fopen(w) failed");
  for (int i = 0; i < 5000; i++)
    fputc((char)('A' + (i % 26)), w);
  CHECK(name, fclose(w) == 0, "fclose failed");

  FILE *r = fopen(TMP, "r");
  CHECK(name, r != NULL, "fopen(r) failed");
  for (int i = 0; i < 5000; i++) {
    int c = fgetc(r);
    if (c != ('A' + (i % 26))) {
      fclose(r);
      bad(name, "byte mismatch on read-back");
      return;
    }
  }
  CHECK(name, fgetc(r) == EOF, "extra bytes after the payload");
  fclose(r);
  ok(name);
}

/* A12 — append mode starts at the end and never truncates. */
static void t_append_mode(void) {
  const char *name = "posix/append-mode";
  CHECK(name, write_file(TMP, "head", 4) == 0, "fixture write failed");
  FILE *f = fopen(TMP, "a");
  CHECK(name, f != NULL, "fopen(a) failed");
  CHECK(name, fwrite("tail", 1, 4, f) == 4, "short fwrite");
  CHECK(name, fclose(f) == 0, "fclose failed");
  char back[16];
  int r = OS1_fs_read(TMP, back, sizeof(back), 0);
  CHECK(name, r == 8, "append did not extend the file");
  CHECK(name, memcmp(back, "headtail", 8) == 0, "append clobbered the head");
  ok(name);
}

/* ---------- group B: the native descriptor layer, on its own -------------- */

/* B1 — open/lseek/read/close, no stdio involved. */
static void t_native_descriptor_roundtrip(void) {
  const char *name = "native/open-lseek-read";
  CHECK(name, write_file(TMP, "0123456789", 10) == 0, "fixture write failed");
  int fd = open(TMP, O_RDONLY);
  CHECK(name, fd >= 0, "open failed");
  char b[4];
  CHECK(name, read(fd, b, 4) == 4, "short read");
  CHECK(name, memcmp(b, "0123", 4) == 0, "wrong prefix");
  CHECK(name, lseek(fd, 6, SEEK_SET) == 6, "lseek did not report the offset");
  CHECK(name, read(fd, b, 4) == 4, "short read after lseek");
  CHECK(name, memcmp(b, "6789", 4) == 0, "lseek landed at the wrong offset");
  CHECK(name, read(fd, b, 4) == 0, "read past the end did not return 0");
  close(fd);
  ok(name);
}

/* B2 — the native positional read's offset contract, which the stream layer is
 * built on: a non-zero offset reads from there, and past the end reads 0. */
static void t_native_positional_offset(void) {
  const char *name = "native/positional-offset";
  CHECK(name, write_file(TMP, "0123456789", 10) == 0, "fixture write failed");
  char b[8];
  CHECK(name, OS1_fs_read(TMP, b, 4, 6) == 4, "offset read short");
  CHECK(name, memcmp(b, "6789", 4) == 0, "offset read wrong bytes");
  CHECK(name, OS1_fs_read(TMP, b, 4, 10) == 0, "read at EOF did not return 0");
  ok(name);
}

/* ---------- group C: the two layers must agree --------------------------- */

/*
 * C1 — stat() and the stream layer must report the same size.  A divergence
 * here is what made the file manager unable to open files: two consumers
 * disagreeing about what the filesystem said.
 */
static void t_stat_agrees_with_stream(void) {
  const char *name = "layers/stat-agrees-with-stream";
  CHECK(name, write_file(TMP, "0123456789ab", 12) == 0, "fixture write failed");
  struct stat st;
  CHECK(name, stat(TMP, &st) == 0, "stat failed");
  CHECK(name, st.st_size == 12, "stat reported the wrong size");
  CHECK(name, S_ISREG(st.st_mode), "stat did not call a file a file");
  FILE *f = fopen(TMP, "r");
  CHECK(name, f != NULL, "fopen failed");
  fseek(f, 0, SEEK_END);
  long via_stream = ftell(f);
  fclose(f);
  CHECK(name, via_stream == st.st_size, "stream and stat disagree on size");
  ok(name);
}

/*
 * C2 — a directory must be distinguishable from a file WITHOUT trying to list
 * it.  The regression this encodes: the distinction used to be inferred from
 * "listing succeeded", an unwritten invariant two callers depended on.
 */
static void t_stat_file_vs_dir(void) {
  const char *name = "layers/stat-file-vs-dir";
  CHECK(name, write_file(TMP, "x", 1) == 0, "fixture write failed");
  struct stat sf, sd;
  CHECK(name, stat(TMP, &sf) == 0, "stat(file) failed");
  CHECK(name, stat("/home", &sd) == 0, "stat(dir) failed");
  CHECK(name, S_ISREG(sf.st_mode), "a file did not report as a file");
  CHECK(name, S_ISDIR(sd.st_mode), "a directory did not report as one");
  CHECK(name, !S_ISDIR(sf.st_mode), "a file reported as a directory");
  ok(name);
}

/* C3 — opendir accepts a directory and refuses a file.  The file manager calls
 * this on whatever the user clicked. */
static void t_opendir_rejects_a_file(void) {
  const char *name = "filemgr/opendir-file-vs-dir";
  CHECK(name, write_file(TMP, "x", 1) == 0, "fixture write failed");
  DIR *d = opendir("/home");
  CHECK(name, d != NULL, "opendir on a directory failed");
  int saw = 0;
  while (readdir(d))
    saw++;
  closedir(d);
  CHECK(name, saw > 0, "directory listed as empty");
  DIR *bad_dir = opendir(TMP);
  if (bad_dir) {
    closedir(bad_dir);
    bad(name, "opendir accepted a regular file");
    return;
  }
  ok(name);
}

/* C4 — the native read and the stream read of a whole file must be identical
 * byte for byte.  This is the direct statement that the POSIX personality does
 * not change what the layer beneath it says. */
static void t_native_and_stream_agree(void) {
  const char *name = "layers/native-equals-stream";
  static char payload[5000];
  for (int i = 0; i < (int)sizeof(payload); i++)
    payload[i] = (char)(i * 7 + (i >> 5));
  CHECK(name, write_file(TMP, payload, sizeof(payload)) == 0,
        "fixture write failed");

  static char via_native[sizeof(payload)];
  CHECK(name,
        OS1_fs_read(TMP, via_native, sizeof(via_native), 0) ==
            (int)sizeof(payload),
        "native read short");

  static char via_stream[sizeof(payload)];
  FILE *f = fopen(TMP, "r");
  CHECK(name, f != NULL, "fopen failed");
  size_t got = fread(via_stream, 1, sizeof(via_stream), f);
  fclose(f);
  CHECK(name, got == sizeof(payload), "stream read short");
  CHECK(name, memcmp(via_native, via_stream, sizeof(payload)) == 0,
        "the two layers returned different bytes");
  ok(name);
}

/* ---------- group D: the CAUSE of a failure, not merely the fact ---------- */

/*
 * D1 — `rm`, and the `rmdir` that does not exist yet, must be able to tell WHY
 * a removal failed.  Every cause in the ext4 driver used to collapse onto a
 * bare -1, and -1 IS -EPERM by value, so "directory not empty" and "no such
 * file" both surfaced as "Operation not permitted" (EXT4-ERRNO-01).
 *
 * It mattered twice over, because OS1_report_error classifies BY errno: a
 * driver failure filed as EPERM raised an amber policy-denial notification
 * whatever had actually gone wrong, while a real EIO never raised the red one.
 *
 * This case is also the precondition for R1b being testable at all: a verb
 * whose refusals are indistinguishable can only be asserted zero/non-zero, so
 * moving the verb first would have produced a new ABI with the old coverage.
 */
static void t_unlink_reports_the_cause(void) {
  const char *name = "rm/unlink-errno-distinguishes";
  const char *dir = "/home/.libctest.d";
  const char *inner = "/home/.libctest.d/f";

  remove(inner); /* leftovers from an interrupted earlier run */
  remove(dir);
  CHECK(name, mkdir(dir, 0755) == 0, "fixture mkdir failed");
  CHECK(name, write_file(inner, "x", 1) == 0, "fixture write failed");

  errno = 0;
  CHECK(name, remove(dir) == -1, "removing a NON-EMPTY directory succeeded");
  CHECK(name, errno == ENOTEMPTY,
        "a non-empty directory did not report ENOTEMPTY");

  errno = 0;
  CHECK(name, remove("/home/.libctest.absent") == -1,
        "removing a missing path succeeded");
  CHECK(name, errno == ENOENT, "a missing path did not report ENOENT");

  /* Emptied, the same directory goes away: the refusal was about the CONTENTS,
   * not about it being a directory — the distinction a caller acts on. */
  CHECK(name, remove(inner) == 0, "removing the inner file failed");
  CHECK(name, remove(dir) == 0, "removing the emptied directory failed");
  ok(name);
}

/*
 * D2 — mkdir must SET errno rather than leave the previous call's value.  It
 * returned a bare -1 without touching errno, so a caller doing the POSIX thing
 * — check -1, then read errno — read whatever happened to be there.  A stale
 * errno is worse than none: it is indistinguishable from a fresh one.  The
 * sentinel below is the whole point of the case.
 */
static void t_mkdir_sets_errno(void) {
  const char *name = "mkdir/sets-errno";
  const char *dir = "/home/.libctest.d2";

  remove(dir);
  CHECK(name, mkdir(dir, 0755) == 0, "fixture mkdir failed");

  errno = EBADF; /* a sentinel that the correct answer is NOT */
  CHECK(name, mkdir(dir, 0755) == -1, "mkdir on an existing path succeeded");
  CHECK(name, errno == EEXIST,
        "mkdir left a stale errno (did not report EEXIST)");

  CHECK(name, remove(dir) == 0, "cleanup failed");
  ok(name);
}

/* Real-world stdio/console contract used by GNU coreutils cat:
 * fstat(1) must identify a CONSOLE without trying lseek() on it. */
static void t_fstat_console_type(void) {
  const char *name = "coreutils/cat-fstat-console";
  struct stat st;
  CHECK(name, fstat(STDOUT_FILENO, &st) == 0,
        "stdout is a valid object but fstat rejected it");
  CHECK(name, S_ISCHR(st.st_mode),
        "stdout is not described as a character stream");
  CHECK(name, st.st_blksize > 0,
        "console lacks an I/O size hint");
  ok(name);
}

/* The pipe objects are distinct capability types and are not seekable.
 * Both ends remain observable by fstat() while lseek() keeps ESPIPE. */
static void t_fstat_pipe_type(void) {
  const char *name = "coreutils/fstat-pipe-ends";
  int fds[2] = {-1,-1};
  CHECK(name, pipe(fds) == 0, "pipe allocation failed");
  struct stat r = {0}, w = {0};
  int a = fstat(fds[0], &r), b = fstat(fds[1], &w);
  int c = close(fds[0]), e = close(fds[1]);
  CHECK(name, a == 0 && b == 0, "valid pipe fstat failed");
  CHECK(name, S_ISFIFO(r.st_mode) && S_ISFIFO(w.st_mode),
        "pipe endpoints are not FIFO streams");
  CHECK(name, c == 0 && e == 0, "pipe descriptor cleanup failed");
  ok(name);
}

/* fstat(file) is an observational object metadata request. It must not
 * change the handle's shared FILE offset, even though old libc used SEEK_END. */
static void t_fstat_keeps_file_offset(void) {
  const char *name = "libc/fstat-preserves-file-offset";
  CHECK(name, write_file(TMP, "01234567", 8) == 0,
        "fixture write failed");
  int fd = open(TMP, O_RDONLY);
  CHECK(name, fd >= 0, "file open failed");
  long position = lseek(fd, 3, SEEK_SET);
  struct stat st;
  int rc = fstat(fd, &st);
  long after = lseek(fd, 0, SEEK_CUR);
  int closed = close(fd);
  CHECK(name, position == 3 && rc == 0 && S_ISREG(st.st_mode),
        "valid file metadata lookup failed");
  CHECK(name, st.st_size == 8 && after == 3,
        "fstat changed the shared file position or size");
  CHECK(name, closed == 0, "file descriptor cleanup failed");
  ok(name);
}

/* Actual parent and logical ownership are different concepts. This
 * compares getppid() with the canonical read-only PROCESS object response,
 * not a constant 1 or a guessed parent from the currently focused window. */
static void t_getppid_matches_process_object(void) {
  const char *name = "libc/getppid-process-object";
  int parent = getppid(), self = getpid();
  CHECK(name, parent > 0 && parent != self,
        "parent is absent or aliases the process itself");
  char key[20], snapshot[256];
  int n = snprintf(key, sizeof(key), "%d", self);
  CHECK(name, n > 0 && (size_t)n < sizeof(key), "PID format failed");
  long fd = OS1low_handle_create(OS1_NS_PROC, key, OS1_RIGHT_READ,
                                 OBJ_TYPE_PROCESS);
  CHECK(name, fd >= 0, "cannot obtain read-only PROCESS capability");
  long got = OS1_object_read((int)fd, snapshot, sizeof(snapshot)-1U);
  long closed = OS1low_handle_close((int)fd);
  CHECK(name, got > 0 && got < (long)sizeof(snapshot) && closed == 0,
        "PROCESS metadata read or close failed");
  snapshot[got] = '\0';
  const char *line = strstr(snapshot, "\nparent=");
  CHECK(name, line != NULL && atoi(line + sizeof("\nparent=")-1U) == parent,
        "getppid disagrees with canonical process lineage");
  ok(name);
}

/* Real SDK execution contract tests: exact resolution, rejection of invalid
 * requests, and a complete native child spawn/wait through the public API. */
static void t_execsvc_resolver(void) {
  const char *name = "execsvc/shared-resolver";
  char path[EXECSVC_PATH_MAX];
  CHECK(name, execsvc_resolve_path("/bin/true", path, sizeof(path)) == 0 &&
              strcmp(path, "/bin/true") == 0, "absolute path changed");
  char cwd[EXECSVC_PATH_MAX], expected[EXECSVC_PATH_MAX];
  CHECK(name, getcwd(cwd, sizeof(cwd)) == 0, "native getcwd status failed");
  int n = snprintf(expected, sizeof(expected), "%s%s./true", cwd,
                   cwd[strlen(cwd) - 1U] == '/' ? "" : "/");
  CHECK(name, n > 0 && (size_t)n < sizeof(expected), "cwd fixture too long");
  CHECK(name, execsvc_resolve_path("./true", path, sizeof(path)) == 0 &&
              strcmp(path, expected) == 0, "relative path lost native cwd");
  CHECK(name, getcwd(cwd, 1U) < 0, "undersized cwd buffer accepted");
  CHECK(name, execsvc_resolve_path("true", path, sizeof(path)) == 1,
        "bare executable not classified");
  CHECK(name, execsvc_resolve_path("~other/hello", path, sizeof(path)) < 0,
        "unsupported user expansion silently accepted");
  CHECK(name, execsvc_resolve_path("/bin/true", path, 4U) < 0,
        "truncated absolute path was accepted");
  ok(name);
}

static void t_getcwd_bounds(void) {
  const char *name = "native/getcwd-bounds";
  unsigned char storage[256];
  for (unsigned i = 0U; i < sizeof(storage); ++i) storage[i] = 0xa5U;
  CHECK(name, getcwd((char *)storage, sizeof(storage)) == 0,
        "large destination failed");
  size_t length = strnlen((char *)storage, 128U);
  CHECK(name, length > 0U && length < 128U, "cwd is unterminated");
  for (size_t i = 0U; i < sizeof(storage); ++i)
    CHECK(name, i <= length || storage[i] == 0xa5U,
          "kernel copied bytes beyond cwd terminator");
  CHECK(name, getcwd((char *)storage, 0U) == -EINVAL,
        "zero capacity accepted");
  CHECK(name, getcwd(NULL, sizeof(storage)) == -EINVAL,
        "null destination accepted");
  ok(name);
}

static void t_execsvc_foreground_child(void) {
  const char *name = "execsvc/foreground-child";
  char *argv[1] = {(char *)"true"};
  char path[EXECSVC_PATH_MAX];
  int pid = execsvc_launch(1, argv, EXECSVC_MODE_FOREGROUND, NULL, 0,
                           path, sizeof(path));
  CHECK(name, pid > 0 && strcmp(path, "/bin/true") == 0,
        "shared resolver did not create the real child");
  int status = 0, joined = 0;
  for (int n = 0; n < 400 && joined == 0; ++n) {
    joined = waitpid(pid, &status, WNOHANG);
    if (joined == 0) OS1_sleep(10);
  }
  CHECK(name, joined == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "child process did not exit cleanly through waitpid");
  ok(name);
}

static void t_execsvc_modes_reject_invalid(void) {
  const char *name = "execsvc/reject-invalid-mode";
  char *argv[1] = {(char *)"/bin/hello"};
  char path[EXECSVC_PATH_MAX];
  int rc = execsvc_launch(1, argv, (enum execsvc_launch_mode)99,
                          NULL, 0, path, sizeof(path));
  CHECK(name, rc == -EINVAL, "invalid launch mode was accepted");
  rc = execsvc_launch(1, argv, EXECSVC_MODE_HOSTED, NULL, 1,
                       path, sizeof(path));
  CHECK(name, rc == -EINVAL, "hosted launch accepted redirection");
  ok(name);
}

static void t_long_double_runtime(void) {
  const char *name = "compiler-runtime/long-double";
  volatile double source = 1.5;
  volatile long double a = (long double)source;
  volatile long double b = 2.0L;
  volatile long double sum = a + b, product = a * b;
  volatile long double difference = b - a, quotient = product / b;
  CHECK(name, (double)a == source && sum == 3.5L && product == 3.0L &&
        difference == 0.5L && quotient == a, "conversion/arithmetic mismatch");
  CHECK(name, a < b && a <= b && b > a && b >= a && a != b &&
        !(a > b) && !(a >= b) && !(b < a) && !(b <= a),
        "comparison helper ABI mismatch");
  volatile unsigned long long integer = 0x100000003ULL;
  volatile long double wide = (long double)integer;
  CHECK(name, (unsigned long long)wide == integer,
        "unsigned 64-bit conversion mismatch");
  volatile long double zero = 0.0L;
  volatile long double nan = zero / zero;
  CHECK(name, nan != nan && !(nan == a) && !(nan < a) && !(nan > a) &&
        !(nan <= a) && !(nan >= a), "unordered comparison mismatch");
  ok(name);
}

int main(void) {
  printf("[libctest] stdio / POSIX-over-native conformance\n");

  t_lua_shebang_getc_then_fread();
  t_fread_then_getc();
  t_fputc_fputs_fwrite_ordering();
  t_interleaved_read_write();
  t_fseek_undoes_ungetc();
  t_ungetc_then_fread();
  t_fgets_across_buffer_boundary();
  t_ftell_tracks_every_path();
  t_eof_and_rewind();
  t_fread_partial_element();
  t_doom_savegame_roundtrip();
  t_append_mode();

  t_native_descriptor_roundtrip();
  t_native_positional_offset();
  t_fstat_console_type();
  t_fstat_pipe_type();
  t_fstat_keeps_file_offset();
  t_getppid_matches_process_object();
  t_execsvc_resolver();
  t_getcwd_bounds();
  t_execsvc_foreground_child();
  t_execsvc_modes_reject_invalid();

  t_stat_agrees_with_stream();
  t_stat_file_vs_dir();
  t_opendir_rejects_a_file();
  t_native_and_stream_agree();

  t_unlink_reports_the_cause();
  t_mkdir_sets_errno();
  t_long_double_runtime();

  OS1_fs_unlink(TMP);
  printf("[libctest] done: %d/%d passed, %d failure(s)\n", g_pass,
         g_pass + g_fail, g_fail);
  return g_fail ? 1 : 0;
}
