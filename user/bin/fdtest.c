/*
 * user/bin/fdtest.c
 * Per-process fd-table test app (ABI-03, epic #93).
 *
 * Exercises the POSIX-style descriptor path end to end against the rootfs
 * file /etc/init.cfg (also used by writetest — config files are writable,
 * /bin and /sys are not):
 *   1. open(O_RDONLY) returns a fd >= 3; sequential read()s advance the
 *      offset (two reads return different, adjacent content);
 *   2. lseek(SEEK_SET 0) rewinds: the next read returns the first bytes
 *      again; lseek(SEEK_END 0) returns the file size (matches the
 *      file_read size probe);
 *   3. open(O_RDWR): write at offset 0 through the fd, lseek back, read
 *      back and compare, then restore the original bytes;
 *   4. denials: open("/bin/shell", O_WRONLY) -> -EACCES (user process),
 *      open missing path -> -ENOENT, read/lseek on a closed fd -> -EBADF,
 *      write on an O_RDONLY fd -> -EBADF, O_CREAT -> -EINVAL.
 * Results go to the window AND the serial console (printf).
 */
#include <fcntl.h>
#include <os1.h>
#include <string.h>

static int failures = 0;

static void check(int win_id, const char *name, int ok) {
  printf_win(win_id, "%s: %s\n", name, ok ? "PASS" : "FAIL");
  printf("[fdtest] %s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok)
    failures++;
}

int main(void) {
  int win_id = create_window(120, 120, 400, 300, "FD Test");
  if (win_id < 0)
    return 1;

  const char *path = "/etc/init.cfg";
  char a[9], b[9], c[9], orig[9];
  int ok;

  printf_win(win_id, "Testing fd table on %s...\n", path);
  printf("[fdtest] target %s\n", path);

  /* 1. open + sequential reads advance the offset */
  int fd = open(path, O_RDONLY);
  ok = fd >= 3;
  if (ok) {
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    ok = read(fd, a, 8) == 8 && read(fd, b, 8) == 8 &&
         memcmp(a, b, 8) != 0; /* adjacent chunks, not the same bytes */
  }
  check(win_id, "open+seq-read", ok);

  /* 2. lseek rewind + SEEK_END == size probe */
  long size = file_read(path, (void *)0, 0, 0);
  memset(c, 0, sizeof(c));
  ok = lseek(fd, 0, SEEK_SET) == 0 && read(fd, c, 8) == 8 &&
       memcmp(a, c, 8) == 0 && lseek(fd, 0, SEEK_END) == size && size > 0;
  check(win_id, "lseek-rewind+end", ok);

  /* 3. read-write fd: overwrite, read back, restore */
  int wfd = open(path, O_RDWR);
  ok = wfd >= 3 && wfd != fd;
  if (ok) {
    memset(orig, 0, sizeof(orig));
    ok = read(wfd, orig, 8) == 8 && lseek(wfd, 0, SEEK_SET) == 0;
    if (ok) {
      write(wfd, "FDTEST!!", 8);
      memset(c, 0, sizeof(c));
      ok = lseek(wfd, 0, SEEK_SET) == 0 && read(wfd, c, 8) == 8 &&
           memcmp(c, "FDTEST!!", 8) == 0;
      /* restore the original bytes whatever happened */
      lseek(wfd, 0, SEEK_SET);
      write(wfd, orig, 8);
    }
  }
  check(win_id, "rdwr-write-readback", ok);

  /* 4a. capability: /bin is read-only for user processes */
  ok = open("/bin/shell", O_WRONLY) == -EACCES;
  check(win_id, "deny-bin-wronly", ok);

  /* 4b. missing path, closed fd, mode and flag misuse */
  ok = open("/no/such/file", O_RDONLY) == -ENOENT;
  check(win_id, "enoent", ok);

  int rfd = open(path, O_RDONLY);
  if (rfd >= 3) {
    /* write on an O_RDONLY fd must not modify the file (the userland
     * write() wrapper returns void, so verify by content) */
    write(rfd, "XXXXXXXX", 8);
  }
  memset(c, 0, sizeof(c));
  ok = rfd >= 3 && file_read(path, c, 8, 0) == 8 && memcmp(c, orig, 8) == 0;
  check(win_id, "deny-write-on-rdonly", ok);

  ok = rfd >= 3 && close(rfd) == 0 && read(rfd, a, 1) == -EBADF &&
       lseek(rfd, 0, SEEK_SET) == -EBADF && close(rfd) == -EBADF;
  check(win_id, "ebadf-after-close", ok);

  ok = open(path, O_RDONLY | O_CREAT) == -EINVAL;
  check(win_id, "einval-o-creat", ok);

  if (fd >= 0)
    close(fd);
  if (wfd >= 0)
    close(wfd);

  printf_win(win_id, "done: %d failure(s)\n", failures);
  printf("[fdtest] done: %d failure(s)\n", failures);

  /* Keep the window up briefly so the result is visible interactively. */
  for (int i = 0; i < 150; i++)
    yield();
  return failures ? 1 : 0;
}
