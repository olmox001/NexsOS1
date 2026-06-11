/*
 * user/bin/writetest.c
 * Filesystem write-path test app.
 *
 * Exercises the three write modes the ext4 provider supports, against the
 * existing rootfs file init.cfg, and verifies each by reading back:
 *   1. overwrite at offset 0 (mapped block, no metadata change);
 *   2. append at EOF (extent image: depth-0 root extend/append;
 *      legacy image: direct-block allocation);
 *   3. sparse far write at 49152 = block 12 (extent image: new extent after
 *      a hole; legacy image: single-indirect pointer-block allocation),
 *      plus a hole readback that must come back as zeros.
 * Results go to the window AND the serial console (printf).
 */
#include <os1.h>

static int failures = 0;

static void check(int win_id, const char *name, int ok) {
  printf_win(win_id, "%s: %s\n", name, ok ? "PASS" : "FAIL");
  printf("[writetest] %s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok)
    failures++;
}

int main(void) {
  int win_id = create_window(100, 100, 400, 300, "Write Test");
  if (win_id < 0) {
    return 1;
  }

  const char *path = "/etc/init.cfg";
  char rbuf[64];

  printf_win(win_id, "Testing file writes on %s...\n", path);
  printf("[writetest] target %s\n", path);

  /* 1. Overwrite at offset 0 (block already mapped). */
  const char *t1 = "# writetest overwrite\n";
  int n = file_write(path, t1, strlen(t1), 0);
  int ok = (n == (int)strlen(t1));
  if (ok) {
    memset(rbuf, 0, sizeof(rbuf));
    ok = file_read(path, rbuf, strlen(t1), 0) == (int)strlen(t1) &&
         strncmp(rbuf, t1, strlen(t1)) == 0;
  }
  check(win_id, "overwrite@0", ok);

  /* 2. Append at EOF (extends the file: new size = old size + marker). */
  int size = file_read(path, NULL, 0, 0);
  const char *t2 = "APPEND-OK";
  ok = size > 0 && file_write(path, t2, strlen(t2), size) == (int)strlen(t2);
  if (ok) {
    memset(rbuf, 0, sizeof(rbuf));
    ok = file_read(path, rbuf, strlen(t2), size) == (int)strlen(t2) &&
         strncmp(rbuf, t2, strlen(t2)) == 0 &&
         file_read(path, NULL, 0, 0) == size + (int)strlen(t2);
  }
  check(win_id, "append@EOF", ok);

  /* 3. Sparse far write at block 12 (49152): hole in between, then the
   *    block past it.  Exercises extent append after a gap on the extents
   *    image and single-indirect allocation on the legacy image. */
  const char *t3 = "FARBLOCK";
  ok = file_write(path, t3, strlen(t3), 49152) == (int)strlen(t3);
  if (ok) {
    memset(rbuf, 0xAA, sizeof(rbuf));
    ok = file_read(path, rbuf, strlen(t3), 49152) == (int)strlen(t3) &&
         strncmp(rbuf, t3, strlen(t3)) == 0;
  }
  if (ok) {
    /* The hole (offset 8192, block 2) must read back as zeros. */
    memset(rbuf, 0xAA, 8);
    ok = file_read(path, rbuf, 8, 8192) == 8;
    for (int i = 0; ok && i < 8; i++)
      if (rbuf[i] != 0)
        ok = 0;
  }
  check(win_id, "sparse@blk12+hole", ok);

  printf_win(win_id, "Done: %d failure(s).\n", failures);
  printf("[writetest] done: %d failure(s)\n", failures);
  return failures ? 1 : 0;
}
