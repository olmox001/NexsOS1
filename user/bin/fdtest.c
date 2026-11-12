/*
 * user/bin/fdtest.c
 * Per-process fd-table test app (ABI-03, epic #93).
 *
 * Exercises the POSIX-style descriptor path end to end against the rootfs
 * dedicated writable /home/shared/fdtest.fixture (writetest uses a separate
 * fixture so offset/sparse tests cannot collide; /sys/bin is machine-only):
 *   1. open(O_RDONLY) returns a fd >= 3; sequential read()s advance the
 *      offset (two reads return different, adjacent content);
 *   2. lseek(SEEK_SET 0) rewinds: the next read returns the first bytes
 *      again; lseek(SEEK_END 0) returns the file size (matches the
 *      file_read size probe);
 *   3. open(O_RDWR): write at offset 0 through the fd, lseek back, read
 *      back and compare, then restore the original bytes;
 *   4. denials: open("/sys/bin/nxshell", O_WRONLY) -> -1/EACCES,
 *      missing path -> -1/ENOENT; closed fd read/lseek/close -> -1/EBADF,
 *      write on O_RDONLY handle leaves bytes unchanged, and invalid flag
 *      bits are refused with -1/EINVAL. Each checks the actual POSIX seam.
 * Results go to the window AND the serial console (printf).
 */
#include <errno.h>
#include <fcntl.h>
#include <os1.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

static void check(int win_id, const char *name, int ok) {
  printf_win(win_id, "%s: %s\n", name, ok ? "PASS" : "FAIL");
  printf("[fdtest] %s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok)
    failures++;
}

static int truncate_descriptor_test(void) {
  const char *path = "/home/shared/fdtest.truncate";
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return 1;
  char bytes[10] = {0};
  int result = write(fd, "0123456789", 10) == 10 ? 0 : 2;
  if (result == 0 && (ftruncate(fd, 4) != 0 ||
      lseek(fd, 0, SEEK_CUR) != 10 || file_read(path, bytes, 10, 0) != 4 ||
      memcmp(bytes, "0123", 4) != 0)) result = 3;
  if (result == 0 && (ftruncate(fd, 9) != 0 ||
      lseek(fd, 0, SEEK_CUR) != 10 || file_read(path, bytes, 10, 0) != 9 ||
      memcmp(bytes, "0123", 4) != 0)) result = 4;
  for (unsigned i = 4U; i < 9U && result == 0; ++i)
    if (bytes[i] != 0) result = 5;
  if (result == 0 && (ftruncate(fd, 0) != 0 ||
      lseek(fd, 0, SEEK_CUR) != 10 || ftruncate(fd, 9) != 0 ||
      file_read(path, bytes, 10, 0) != 9)) result = 6;
  for (unsigned i = 0U; i < 9U && result == 0; ++i)
    if (bytes[i] != 0) result = 7;
  if (close(fd) != 0) result = 8;
  fd = open(path, O_RDONLY);
  errno = 0;
  if (fd < 0 || ftruncate(fd, 0) != -1 || errno != EBADF) result = 9;
  if (fd >= 0 && close(fd) != 0) result = 10;
  if (OS1_fs_unlink(path) != 0) result = 11;
  return result;
}

int main(void) {
  int win_id = create_window(120, 120, 400, 300, "FD Test");
  if (win_id < 0)
    return 1;

  const char *path = "/home/shared/fdtest.fixture";
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
           memcmp(c, "FDTEST!!", 8) == 0 &&
           lseek(wfd, 0, SEEK_END) == size;
      /* restore the original bytes whatever happened */
      lseek(wfd, 0, SEEK_SET);
      write(wfd, orig, 8);
    }
  }
  check(win_id, "rdwr-write-readback", ok);
  check(win_id, "truncate-writeonly-position-and-zero-tail",
        truncate_descriptor_test() == 0);

  /* 4a. capability: /bin is read-only for user processes */
  errno = 0;
  int rejected = open("/sys/bin/nxshell", O_WRONLY);
  ok = rejected == -1 && errno == EACCES;
  if (rejected >= 0)
    close(rejected);
  check(win_id, "deny-bin-wronly", ok);

  /* 4b. missing path, closed fd, mode and flag misuse */
  errno = 0;
  int missing = open("/no/such/file", O_RDONLY);
  ok = missing == -1 && errno == ENOENT;
  if (missing >= 0)
    close(missing);
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

  int originally_closed = rfd >= 3 && close(rfd) == 0;
  errno = 0;
  long invalid_read = read(rfd, a, 1);
  int read_errno = errno;
  errno = 0;
  long invalid_seek = lseek(rfd, 0, SEEK_SET);
  int seek_errno = errno;
  errno = 0;
  int invalid_close = close(rfd);
  int close_errno = errno;
  ok = originally_closed && invalid_read == -1 && read_errno == EBADF &&
       invalid_seek == -1 && seek_errno == EBADF &&
       invalid_close == -1 && close_errno == EBADF;
  check(win_id, "ebadf-after-close", ok);

  /* O_CREAT on an existing file is valid POSIX. Test a genuinely invalid,
   * unsupported bit and require the exact POSIX errno mapping. */
  errno = 0;
  int invalid_flags = open(path, O_RDONLY | 0x40000000);
  ok = invalid_flags == -1 && errno == EINVAL;
  if (invalid_flags >= 0)
    close(invalid_flags);
  check(win_id, "invalid-flags-not-ignored", ok);

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
