/*
 * user/lib.c
 * User-space library - delegates to syscall.S wrappers
 */
#include <os1.h>

size_t strlen(const char *s) {
  size_t len = 0;
  while (*s++)
    len++;
  return len;
}

/*
 * User Library Implementation
 * All syscalls now delegate to _sys_* wrappers in syscall.S
 */

long read(int fd, char *buf, unsigned long count) {
  return _sys_read(fd, buf, count);
}

/* Stack protector support */
uintptr_t __stack_chk_guard = 0x595e9eda;

void __stack_chk_fail(void);

void __stack_chk_fail(void) {
  printf("Stack smashing detected!\n");
  exit(1);
}

void *memset(void *s, int c, size_t n) {
  unsigned char *p = s;
  while (n--)
    *p++ = (unsigned char)c;
  return s;
}

void write(int fd, const char *buf, size_t count) {
  _sys_write(fd, buf, count);
}

long get_time(void) { return _sys_get_time(); }

int get_pid(void) { return _sys_get_pid(); }

void exit(int status) {
  _sys_exit(status);
  while (1)
    ; /* Should not return */
}

int spawn(const char *path) { return _sys_spawn(path); }

int kill_process(int pid) { return _sys_kill(pid); }

int wait(int pid) { return _sys_wait(pid); }

void draw(int x, int y, int w, int h, int color) {
  _sys_draw(x, y, w, h, color);
}

void flush(void) { _sys_flush(); }

int create_window(int x, int y, int w, int h, const char *title) {
  return _sys_create_window(x, y, w, h, title);
}

void destroy_window(int win_id) { _sys_destroy_window(win_id); }

void window_draw(int win_id, int x, int y, int w, int h, unsigned int color) {
  _sys_window_draw(win_id, x, y, w, h, color);
}

void window_blit(int win_id, int x, int y, int w, int h,
                 const unsigned int *buf) {
  _sys_window_blit(win_id, x, y, w, h, buf);
}

void yield(void) { _sys_yield(); }

void sleep(int ticks) {
  long end = get_time() + ticks;
  while (get_time() < end) {
    yield();
  }
}

void compositor_render(void) { _sys_compositor_render(); }

int send(int pid, struct ipc_message *msg) { return _sys_send(pid, msg); }

int recv(int pid, struct ipc_message *msg) { return _sys_recv(pid, msg); }

int notify(const char *title, const char *msg) {
  struct ipc_message imsg;
  imsg.type = IPC_TYPE_NOTIFY;
  imsg.data1 = 0;
  imsg.data2 = 0;

  /* Pack title and msg into payload (simple space-separated for now) */
  int i = 0;
  while (*title && i < 30) {
    imsg.payload[i++] = *title++;
  }
  imsg.payload[i++] = ':';
  imsg.payload[i++] = ' ';
  while (*msg && i < 63) {
    imsg.payload[i++] = *msg++;
  }
  imsg.payload[i] = '\0';

  /* Notification server PID is fetched from registry */
  char pid_buf[16];
  int pid = 2; /* Default fallback */
  if (registry_read("srv.notify_pid", pid_buf, sizeof(pid_buf)) == 0) {
    /* Simple atoi */
    pid = 0;
    for (int j = 0; pid_buf[j] != '\0'; j++) {
      if (pid_buf[j] >= '0' && pid_buf[j] <= '9') {
        pid = pid * 10 + (pid_buf[j] - '0');
      }
    }
  }

  return send(pid, &imsg);
}

int try_recv(int pid, struct ipc_message *msg) {
  extern int _sys_try_recv(int pid, void *msg);
  return _sys_try_recv(pid, msg);
}

void set_window_flags(int win_id, int flags) {
  _sys_window_set_flags(win_id, flags);
}

void set_focus(int pid) {
  extern void _sys_set_focus(int pid);
  _sys_set_focus(pid);
}

void print(const char *s) {
  size_t len = 0;
  while (s[len])
    len++;
  write(1, s, len);
}

void print_hex(unsigned long val) {
  char buf[18];
  buf[0] = '0';
  buf[1] = 'x';
  int i;
  for (i = 0; i < 16; i++) {
    int digit = (val >> ((15 - i) * 4)) & 0xF;
    buf[2 + i] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
  }
  write(1, buf, 18);
}

/* vsnprintf implementation is shared with kernel */
#include "../../kernel/lib/vsnprintf.c"

void vsprintf(char *out, const char *fmt, va_list args) {
  /* Legacy wrapper - unsafe, but needed for backward compat if any calls
   * remain. We assume a large buffer if called directly. Ideally deprecated.
   */
  vsnprintf(out, 65536, fmt, args);
}

void printf(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  write(1, buf, strlen(buf));
}

void printf_win(int win_id, const char *fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  _sys_write(win_id, buf, strlen(buf));
}

void sprintf(char *out, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  /* Still unsafe logic, user passes buffer. Forward to vsprintf */
  vsprintf(out, fmt, args);
  va_end(args);
}

void snprintf(char *out, size_t size, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(out, size, fmt, args);
  va_end(args);
}

/*
 * Standard IO Implementation
 */
int getchar(void) {
  char c;
  if (read(0, &c, 1) == 1) {
    return (unsigned char)c;
  }
  return -1;
}

int putchar(int c) {
  char ch = (char)c;
  write(1, &ch, 1);
  return c;
}

char *gets(char *s, int size) {
  int i = 0;
  while (i < size - 1) {
    int c = getchar();
    if (c < 0)
      break; /* Error */

    if (c == '\b' || c == 127) {
      /* Backspace */
      if (i > 0) {
        i--;
        /* Erase on screen */
        write(1, "\b \b", 3);
      }
      continue;
    }

    /* Echo */
    putchar(c);

    if (c == '\n' || c == '\r') {
      s[i] = '\0';
      return s;
    }
    s[i++] = (char)c;
  }
  s[i] = '\0';
  return s;
}

/* Math implementation is shared with kernel */
#include "../../kernel/lib/math.c"

/* Registry Wrappers */
int registry_read(const char *key, char *buf, size_t size) {
  /* op=0 (READ) */
  return (int)_sys_registry(0, key, buf, size);
}

int registry_write(const char *key, const char *value) {
  /* op=1 (WRITE) */
  /* We cast const char* to char* because syscall signature is generic, but
   * kernel treats it safely */
  return (int)_sys_registry(1, key, (char *)value, 0);
}

int file_write(const char *path, const void *buf, int size, int offset) {
  return _sys_file_write(path, buf, size, offset);
}

int file_read(const char *path, void *buf, int size, int offset) {
  return _sys_file_read(path, buf, size, offset);
}
