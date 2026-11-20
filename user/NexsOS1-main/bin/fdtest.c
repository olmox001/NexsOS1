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
#include <poll.h>
#include <sys/wait.h>
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

static int nonblock_file_test(void) {
  const char *path = "/home/shared/fdtest.nonblock";
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK, 0644);
  if (fd < 0) return 1;
  int result = write(fd, "abcdef", 6) == 6 && ftruncate(fd, 3) == 0 ? 0 : 2;
  if (close(fd) != 0) result = 3;
  char bytes[8] = {0};
  fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0 || read(fd, bytes, sizeof(bytes)) != 3 ||
      memcmp(bytes, "abc", 3) != 0) result = 4;
  if (fd >= 0 && close(fd) != 0) result = 5;
  errno = 0;
  fd = open(path, O_NONBLOCK | O_ACCMODE | O_TRUNC);
  if (fd >= 0 || errno != EINVAL || file_read(path, NULL, 0, 0) != 3)
    result = 6;
  if (fd >= 0 && close(fd) != 0) result = 7;
  if (OS1_fs_unlink(path) != 0) result = 8;
  return result;
}

static int nonblock_type_denial(void) {
  char path[32];
  int length = snprintf(path, sizeof(path), "/proc/%d", get_pid());
  if (length <= 0 || (size_t)length >= sizeof(path)) return 1;
  errno = 0;
  int fd = open(path, O_RDONLY | O_NONBLOCK);
  int error = errno;
  if (fd >= 0 && close(fd) != 0) return 2;
  return fd == -1 && error == ENOTSUP ? 0 : 3;
}

/* Real guest PIPE readiness: no fake poll return, no loss of bytes, no
 * falsely hidden EOF. Runs through kernel capability table and PIPE ops. */
static int pipe_poll_real_test(void) {
  int fds[2] = {-1, -1};
  if (pipe(fds) != 0) return 1;
  struct os1_pipe_status_v1 state = {
      .abi_version = OS1_PIPE_STATUS_VERSION, .struct_size = sizeof(state)
  };
  int failure = OS1_object_ctl(fds[0], OBJ_CTL_PIPE_STATUS,
                               (long)(uintptr_t)&state) != 0 ||
                state.available != 0U || state.readers != 1U ||
                state.writers != 1U ? 2 : 0;
  struct pollfd reader = {.fd = fds[0], .events = POLLIN};
  struct pollfd writer = {.fd = fds[1], .events = POLLOUT};
  if (failure == 0 && (poll(&reader, 1U, 0) != 0 ||
      poll(&writer, 1U, 0) != 1 ||
      (writer.revents & POLLOUT) == 0)) failure = 3;
  if (failure == 0 && write(fds[1], "abc", 3) != 3) failure = 4;
  if (failure == 0 && (poll(&reader, 1U, 0) != 1 ||
      (reader.revents & POLLIN) == 0)) failure = 5;
  char bytes[4] = {0};
  if (failure == 0 && (read(fds[0], bytes, 3) != 3 ||
      memcmp(bytes, "abc", 3) != 0)) failure = 6;
  if (close(fds[1]) != 0) failure = 7;
  if (failure == 0 && (poll(&reader, 1U, 0) != 1 ||
      (reader.revents & POLLHUP) == 0 ||
      (reader.revents & POLLIN) == 0 ||
      read(fds[0], bytes, 1) != 0)) failure = 8;
  if (close(fds[0]) != 0) failure = 9;
  return failure;
}

/* Negative query and poll validation against REAL invalid and stale handles. */
static int pipe_poll_denial_test(void) {
  int fds[2] = {-1, -1};
  if (pipe(fds) != 0) return 1;
  struct os1_pipe_status_v1 state = {
      .abi_version = OS1_PIPE_STATUS_VERSION + 1U,
      .struct_size = sizeof(state)
  };
  int failed = OS1_object_ctl(fds[0], OBJ_CTL_PIPE_STATUS,
                              (long)(uintptr_t)&state) != -EINVAL ? 2 : 0;
  state.abi_version = OS1_PIPE_STATUS_VERSION;
  state.struct_size = 0U;
  if (failed == 0 && OS1_object_ctl(fds[0], OBJ_CTL_PIPE_STATUS,
                                  (long)(uintptr_t)&state) != -EINVAL)
    failed = 3;
  state.struct_size = sizeof(state);
  if (failed == 0 && OS1_object_ctl(fds[0], OBJ_CTL_PIPE_STATUS,
                                  0L) != -EFAULT)
    failed = 4;
  if (close(fds[1]) != 0 || close(fds[0]) != 0) failed = 5;
  struct pollfd invalid = {.fd = fds[0], .events = POLLIN};
  if (failed == 0 && (poll(&invalid, 1U, 0) != 1 ||
      (invalid.revents & POLLNVAL) == 0)) failed = 6;
  errno = 0;
  if (failed == 0 && (poll(NULL, 1U, 0) != -1 || errno != EINVAL))
    failed = 7;
  return failed;
}

/* POSIX aliases share one real capability, not the previous numeric shim. */
static int fd_dup_shared_pipe_test(void) {
  int fd = dup(1);
  if (fd < 3 || fd == 1) return 1;
  long original = OS1low_cap_query(1);
  long copy = OS1low_cap_query(fd);
  if (original < 0 || copy < 0 ||
      OS1_CAPQ_TYPE(copy) != OBJ_TYPE_CONSOLE ||
      (OS1_CAPQ_RIGHTS(copy) & OS1_RIGHT_WRITE) == 0U) return 2;
  if (close(fd) != 0 || OS1low_cap_query(1) < 0) return 3;
  int pipefd[2] = {-1, -1};
  if (pipe(pipefd) != 0) return 4;
  int writer = dup(pipefd[1]);
  if (writer < 3 || writer == pipefd[1]) return 5;
  if (close(pipefd[1]) != 0 || write(writer, "xy", 2) != 2)
    return 6;
  char buffer[3] = {0};
  if (read(pipefd[0], buffer, 2) != 2 ||
      memcmp(buffer, "xy", 2) != 0) return 7;
  if (close(writer) != 0 || read(pipefd[0], buffer, 1) != 0)
    return 8;
  if (close(pipefd[0]) != 0) return 9;
  return 0;
}

/* dup2 replaces the EXACT destination; F_DUPFD respects the minimum and
 * neither operation may impersonate a foreign fd or fabricate success. */
static int fd_dup2_and_fcntl_test(void) {
  const char *path = "/home/shared/fdtest.duptarget";
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 3) return 1;
  int pipefd[2] = {-1, -1};
  if (pipe(pipefd) != 0) return 2;
  int high = fcntl(pipefd[1], F_DUPFD, 15);
  if (high < 15 || high >= 32) return 3;
  if (dup2(pipefd[1], fd) != fd ||
      OS1_CAPQ_TYPE(OS1low_cap_query(fd)) != OBJ_TYPE_PIPE)
    return 4;
  if (dup2(fd, fd) != fd) return 5;
  if (close(pipefd[1]) != 0 || close(high) != 0) return 6;
  if (write(fd, "ok", 2) != 2) return 7;
  char data[3] = {0};
  if (read(pipefd[0], data, 2) != 2 ||
      memcmp(data, "ok", 2) != 0) return 8;
  errno = 0;
  if (dup2(-1, fd) != -1 || errno != EBADF) return 9;
  errno = 0;
  if (fcntl(fd, F_DUPFD, -1) != -1 || errno != EINVAL)
    return 10;
  errno = 0;
  if (fcntl(fd, F_GETFL) != -1 || errno != ENOTSUP)
    return 11;
  if (close(fd) != 0 || close(pipefd[0]) != 0) return 12;
  if (OS1_fs_unlink(path) != 0) return 13;
  return 0;
}

/* /dev/null is not an ext4 regular file. It must be an actual capability
 * resource: read EOF, discard writes, duplicate through the same fd table. */
static int fd_null_resource_test(void) {
  int input = open("/dev/null", O_RDONLY);
  if (input < 0) return 1;
  char marker[4] = {'a','b','c','d'};
  if (read(input, marker, sizeof(marker)) != 0 ||
      marker[0] != 'a') return 2;
  if (close(input) != 0) return 3;
  int sink = open("/dev/null", O_WRONLY);
  if (sink < 0 || OS1_CAPQ_TYPE(OS1low_cap_query(sink)) !=
      OBJ_TYPE_RESOURCE) return 4;
  if (write(sink, "discarded", 9) != 9) return 5;
  int copy = dup(sink);
  if (copy < 0 || close(sink) != 0) return 6;
  if (write(copy, "xyz", 3) != 3 || close(copy) != 0)
    return 7;
  return 0;
}

/* The NULL provider does not treat a normal file-create request as a
 * success, and rejects invented rights like DESTROY. */
static int fd_null_denials_test(void) {
  errno = 0;
  int fd = open("/dev/null", O_WRONLY | O_CREAT);
  if (fd >= 0 || errno != ENOTSUP) return 1;
  errno = 0;
  fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
  if (fd >= 0 || errno != ENOTSUP) return 2;
  long cap = OS1low_handle_create(OS1_NS_RESOURCE, OS1_RESOURCE_NULL,
                                  OS1_RIGHT_DESTROY, OBJ_TYPE_RESOURCE);
  if (cap != -EINVAL) return 3;
  cap = OS1low_handle_create(OS1_NS_RESOURCE, OS1_RESOURCE_NULL,
                             OS1_RIGHT_READ, OBJ_TYPE_RESOURCE);
  if (cap < 0) return 4;
  if (OS1_object_read((int)cap, NULL, 0) != 0 ||
      OS1low_handle_close((int)cap) != 0) return 5;
  return 0;
}

/* A pipe's last writer must close at the CHILD'S EXIT, not only after the
 * parent collects/reaps its process. Test both payload+EOF and empty EOF. */
static int fd_pipe_child_exit_test(int with_data) {
  int ends[2] = {-1, -1};
  if (pipe(ends) != 0) return 1;
  struct spawn_redir output = {.child_fd = 1, .parent_fd = ends[1]};
  char executable_echo[] = "/bin/echo";
  char executable_true[] = "/bin/true";
  char text[] = "pipe-child-exit";
  char *program = with_data != 0 ? executable_echo : executable_true;
  char *args[] = {program, text};
  long child = OS1low_process_spawn_redir(program,
                  with_data ? 2 : 1, args, 0U, &output, 1);
  if (close(ends[1]) != 0 || child <= 0) {
    if (ends[0] >= 0) close(ends[0]);
    return 2;
  }
  char output_buf[64] = {0};
  size_t total = 0U;
  int eof = 0;
  for (unsigned i = 0U; i < 400U && eof == 0; ++i) {
    struct pollfd event = {.fd = ends[0], .events = POLLIN};
    int ready = poll(&event, 1U, 10);
    if (ready < 0 || (event.revents & (POLLERR | POLLNVAL)) != 0)
      return 3;
    if ((event.revents & POLLIN) != 0) {
      if (total >= sizeof(output_buf) - 1U) return 4;
      long n = read(ends[0], output_buf + total,
                    sizeof(output_buf) - 1U - total);
      if (n < 0) return 5;
      if (n == 0) eof = 1;
      if (n > 0) total += (size_t)n;
    } else if ((event.revents & POLLHUP) != 0) {
      eof = 1;
    }
  }
  if (close(ends[0]) != 0 || eof == 0) return 6;
  int status = 0;
  int exited = 0;
  for (unsigned j = 0U; j < 200U && exited == 0; ++j) {
    int got = waitpid((int)child, &status, WNOHANG);
    if (got == (int)child) exited = 1;
    else if (got < 0) return 7;
    else OS1_sleep(10);
  }
  if (exited == 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return 8;
  if (with_data != 0 && (total < 15U ||
      strstr(output_buf, "pipe-child-exit") == NULL)) return 9;
  if (with_data == 0 && total != 0U) return 10;
  return 0;
}

/* Test the real POSIX fd=0 CONSOLE path, separately from raw keyboard and
 * focus ownership. Parent-to-own-child IPC_INPUT is permitted by the existing
 * input authority policy; no CAP_KEYBOARD_RAW, machine role or grant needed.
 *
 * The key's hardware code is deliberately distinct from the translated UTF8
 * payload. The child must read 'x', never the HID/virtual keycode. */
/* Manual GUI/nxadb foreground transport check. Unlike the parent-child
 * synthetic regression this observes actual keyboard service -> nxttyd ->
 * nxshell -> foreground child -> POSIX read(0), with no extra grants.
 * Invoke as /bin/fdtest --console-manual-probe; press z in its TTY. */
static int fd_console_manual_probe(void) {
  int terminal = isatty(STDIN_FILENO);
  if (printf("[fdtest][manual-tty] ready pid=%d parent=%d isatty=%d\n",
             get_pid(), getppid(), terminal) < 0)
    return 30;
  if (!terminal) return 31;
  for (unsigned i = 0U; i < 10000U; ++i) {
    char character = 0;
    errno = 0;
    long length = read(STDIN_FILENO, &character, 1U);
    if (length == 1) {
      int valid = character == 'z';
      if (printf("[fdtest][manual-tty] byte=%u expected=%u %s\n",
                 (unsigned int)(unsigned char)character, (unsigned int)'z',
                 valid ? "PASS" : "FAIL") < 0) return 32;
      return valid ? 0 : 33;
    }
    if (length != -1 || (errno != EAGAIN && errno != EINTR)) {
      if (printf("[fdtest][manual-tty] read=%ld errno=%d FAIL\n",
                 length, errno) < 0) return 34;
      return 35;
    }
    OS1_sleep(1);
  }
  return 36;
}

static int fd_console_input_child(void) {
  if (!isatty(STDIN_FILENO)) return 11;
  for (unsigned step = 0U; step < 2000U; ++step) {
    char character = 0;
    errno = 0;
    long got = read(STDIN_FILENO, &character, 1);
    if (got == 1) {
      int ok = character == 'x';
      int reported = printf(
          "[fdtest][console-child] read=%d char=%u expected=%u %s\n",
          (int)got, (unsigned int)(unsigned char)character,
          (unsigned int)'x', ok ? "PASS" : "FAIL");
      return reported < 0 ? 15 : (ok ? 0 : 12);
    }
    if (got != -1 || (errno != EAGAIN && errno != EINTR))
      return 13;
    OS1_sleep(1);
  }
  return 14;
}

/* A single event must survive two one-byte object reads, or one two-byte
 * POSIX read. Testing the actual process-scoped console capability catches
 * UTF-8 continuation loss without any raw keyboard grants. */
static int fd_console_utf8_child(int bulk) {
  char bytes[2] = {0, 0};
  if (!isatty(STDIN_FILENO)) return 21;
  if (bulk) {
    long count = read(STDIN_FILENO, bytes, 2U);
    return count == 2 && (unsigned char)bytes[0] == 0xC3U &&
           (unsigned char)bytes[1] == 0xA0U ? 0 : 22;
  }
  unsigned have = 0U;
  for (unsigned attempt = 0U; attempt < 500U && have < 2U; ++attempt) {
    long count = OS1_object_read(STDIN_FILENO, bytes + have, 1U);
    if (count == 1) ++have;
    else if (count != -EAGAIN) return 23;
    if (have < 2U) OS1_sleep(1);
  }
  return have == 2U && (unsigned char)bytes[0] == 0xC3U &&
         (unsigned char)bytes[1] == 0xA0U ? 0 : 24;
}

static int fd_console_send_and_wait(int child, struct ipc_message *message) {
  int delivered = 0;
  for (unsigned attempt = 0U; attempt < 40U && !delivered; ++attempt) {
    long sent = OS1low_ipc_send(child, message);
    if (sent == 0) delivered = 1;
    else OS1_sleep(1);
  }
  if (!delivered)
    return kill_process(child) >= 0 ? 2 : 6;
  for (unsigned step = 0U; step < 300U; ++step) {
    int status = 0;
    int wait = waitpid(child, &status, WNOHANG);
    if (wait == child)
      return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 3;
    if (wait < 0) return 4;
    OS1_sleep(10);
  }
  return kill_process(child) >= 0 ? 5 : 6;
}

static int fd_console_utf8_transport_test(int bulk) {
  char child_path[] = "/bin/fdtest";
  char mode_bulk[] = "--console-utf8-bulk-child";
  char mode_bytes[] = "--console-utf8-byte-child";
  char *args[] = {child_path, bulk ? mode_bulk : mode_bytes, NULL};
  int child = spawn_args(child_path, 2, args);
  if (child <= 0) return 1;
  struct ipc_message key = {0};
  key.type = IPC_TYPE_INPUT;
  key.data1 = 40U;
  key.data2 = 1U;
  key.payload[0] = (char)0xC3U;
  key.payload[1] = (char)0xA0U;
  return fd_console_send_and_wait(child, &key);
}

static int fd_console_input_transport_test(int delay_ms) {
  char child_path[] = "/bin/fdtest";
  char mode[] = "--console-read-child";
  char *args[] = {child_path, mode, NULL};
  int child = spawn_args(child_path, 2, args);
  if (child <= 0) return 1;
  /* A delayed send exercises wakeup of a child already blocked in read(0).
   * An immediate send only proves that the queue can retain a character. */
  if (delay_ms > 0) OS1_sleep(delay_ms);
  struct ipc_message key = {0};
  key.type = IPC_TYPE_INPUT;
  key.data1 = UINT64_C(0x04); /* hardware keycode intentionally not ASCII */
  key.data2 = 1U;
  key.payload[0] = 'x';
  return fd_console_send_and_wait(child, &key);
}

int main(int argc, char *argv[]) {
  if (argc == 2 && strcmp(argv[1], "--console-manual-probe") == 0)
    return fd_console_manual_probe();
  if (argc == 2 && strcmp(argv[1], "--console-read-child") == 0)
    return fd_console_input_child();
  if (argc == 2 && strcmp(argv[1], "--console-utf8-bulk-child") == 0)
    return fd_console_utf8_child(1);
  if (argc == 2 && strcmp(argv[1], "--console-utf8-byte-child") == 0)
    return fd_console_utf8_child(0);

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

  /* Independent descriptor slots share the same open file offset. */
  int copy = dup(fd);
  int target = open(path, O_RDONLY);
  ok = copy >= 0 && target >= 0 && dup2(copy, target) == target &&
       lseek(fd, 0, SEEK_SET) == 0 && read(copy, c, 8) == 8 &&
       memcmp(a, c, 8) == 0 && read(target, c, 8) == 8 &&
       memcmp(b, c, 8) == 0 && lseek(fd, 0, SEEK_CUR) == 16;
  if (target >= 0) close(target);
  if (copy >= 0) close(copy);
  check(win_id, "dup-and-dup2-share-file-position", ok);

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
  check(win_id, "nonblock-file-read-write-and-truncate", nonblock_file_test() == 0);
  check(win_id, "nonblock-process-refused", nonblock_type_denial() == 0);
  check(win_id, "pipe-poll-readable-writable-eof",
        pipe_poll_real_test() == 0);
  check(win_id, "pipe-poll-invalid-version-and-stale",
        pipe_poll_denial_test() == 0);
  check(win_id, "real-dup-stdio-and-shared-pipe",
        fd_dup_shared_pipe_test() == 0);
  check(win_id, "real-dup2-target-and-fcntl-minimum",
        fd_dup2_and_fcntl_test() == 0);
  check(win_id, "native-null-resource-EOF-and-sink",
        fd_null_resource_test() == 0);
  check(win_id, "native-null-resource-authority-denials",
        fd_null_denials_test() == 0);
  check(win_id, "pipe-child-exit-drains-and-signals-EOF",
        fd_pipe_child_exit_test(1) == 0);
  check(win_id, "pipe-child-no-output-signals-EOF",
        fd_pipe_child_exit_test(0) == 0);
  check(win_id, "console-read-utf8-not-keycode",
        fd_console_input_transport_test(0) == 0);
  check(win_id, "console-read-wakes-blocked-child",
        fd_console_input_transport_test(150) == 0);
  check(win_id, "console-read-utf8-bulk", fd_console_utf8_transport_test(1) == 0);
  check(win_id, "console-read-utf8-bytewise", fd_console_utf8_transport_test(0) == 0);

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
