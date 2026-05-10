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

/*
 * vsnprintf: Safe version of vsprintf with buffer limit
 */
static void itoa_buf(char *buf, size_t *idx, size_t size, long d, int base,
                     int width, char pad, int left) {
  char tmp[64];
  int i = 0;
  int neg = 0;
  unsigned long u = d;

  if (base == 10 && d < 0) {
    neg = 1;
    u = -d;
  }

  if (u == 0) {
    tmp[i++] = '0';
  } else {
    while (u > 0) {
      int rem = u % base;
      tmp[i++] = (rem < 10) ? (rem + '0') : (rem - 10 + 'a');
      u /= base;
    }
  }

  if (neg && pad == '0') {
    if (*idx < size - 1)
      buf[(*idx)++] = '-';
    neg = 0;
    if (width > 0)
      width--;
  }

  int len = i + (neg ? 1 : 0);
  if (!left) {
    while (len < width && *idx < size - 1) {
      buf[(*idx)++] = pad;
      len++;
    }
  }

  if (neg && *idx < size - 1)
    buf[(*idx)++] = '-';

  while (i > 0 && *idx < size - 1) {
    buf[(*idx)++] = tmp[--i];
  }

  if (left) {
    while (len < width && *idx < size - 1) {
      buf[(*idx)++] = ' ';
      len++;
    }
  }
}

void vsnprintf(char *out, size_t size, const char *fmt,
               __builtin_va_list args) {
  size_t out_idx = 0;
  if (size == 0)
    return;

  for (const char *p = fmt; *p; p++) {
    if (out_idx >= size - 1)
      break;

    if (*p != '%') {
      out[out_idx++] = *p;
      continue;
    }

    p++; /* Skip % */
    if (*p == '%') {
      out[out_idx++] = '%';
      continue;
    }

    int left = 0;
    char pad = ' ';
    if (*p == '-') {
      left = 1;
      p++;
    }
    if (*p == '0') {
      pad = '0';
      p++;
    }

    int width = 0;
    while (*p >= '0' && *p <= '9') {
      width = width * 10 + (*p - '0');
      p++;
    }

    int is_long = 0;
    if (*p == 'l') {
      is_long = 1;
      p++;
    }

    if (*p == 's') {
      const char *s = __builtin_va_arg(args, const char *);
      if (!s)
        s = "(null)";
      int len = 0;
      while (s[len])
        len++;

      if (!left) {
        while (len < width && out_idx < size - 1) {
          out[out_idx++] = ' ';
          len++;
        }
      }
      while (*s && out_idx < size - 1) {
        out[out_idx++] = *s++;
      }
      if (left) {
        while (len < width && out_idx < size - 1) {
          out[out_idx++] = ' ';
          len++;
        }
      }
    } else if (*p == 'd' || *p == 'i') {
      long d =
          is_long ? __builtin_va_arg(args, long) : __builtin_va_arg(args, int);
      itoa_buf(out, &out_idx, size, d, 10, width, pad, left);
    } else if (*p == 'u') {
      unsigned long u = is_long ? __builtin_va_arg(args, unsigned long)
                                : __builtin_va_arg(args, unsigned int);
      itoa_buf(out, &out_idx, size, (long)u, 10, width, pad, left);
    } else if (*p == 'x' || *p == 'p') {
      unsigned long x;
      if (*p == 'p') {
        x = (unsigned long)__builtin_va_arg(args, void *);
        if (out_idx < size - 1)
          out[out_idx++] = '0';
        if (out_idx < size - 1)
          out[out_idx++] = 'x';
        itoa_buf(out, &out_idx, size, x, 16, width - 2, pad, left);
      } else {
        x = is_long ? __builtin_va_arg(args, unsigned long)
                    : __builtin_va_arg(args, unsigned int);
        itoa_buf(out, &out_idx, size, x, 16, width, pad, left);
      }
    } else if (*p == 'c') {
      int c = __builtin_va_arg(args, int);
      out[out_idx++] = (char)c;
    } else {
      /* Unknown specifier, just copy character */
      out[out_idx++] = *p;
    }
  }
  out[out_idx] = '\0';
}

void vsprintf(char *out, const char *fmt, __builtin_va_list args) {
  /* Legacy wrapper - unsafe, but needed for backward compat if any calls
   * remain. We assume a large buffer if called directly. Ideally deprecated.
   */
  vsnprintf(out, 65536, fmt, args);
}

void printf(const char *fmt, ...) {
  char buf[256];
  __builtin_va_list args;
  __builtin_va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  __builtin_va_end(args);
  write(1, buf, strlen(buf));
}

void printf_win(int win_id, const char *fmt, ...) {
  char buf[512];
  __builtin_va_list args;
  __builtin_va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  __builtin_va_end(args);
  _sys_write(win_id, buf, strlen(buf));
}

void sprintf(char *out, const char *fmt, ...) {
  __builtin_va_list args;
  __builtin_va_start(args, fmt);
  /* Still unsafe logic, user passes buffer. Forward to vsprintf */
  vsprintf(out, fmt, args);
  __builtin_va_end(args);
}

void snprintf(char *out, size_t size, const char *fmt, ...) {
  __builtin_va_list args;
  __builtin_va_start(args, fmt);
  vsnprintf(out, size, fmt, args);
  __builtin_va_end(args);
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

/* Fixed-point multiplication (16.16) */
int fixmul(int a, int b) {
  long long res = (long long)a * b;
  return (int)(res >> 16);
}

/* User-space fixed-point Sine (16.16) */
int sin_fp(int x) {
  const int PI = 205887;
  const int TWO_PI = 411775;

  /* Range reduction to -PI to PI */
  while (x > PI)
    x -= TWO_PI;
  while (x < -PI)
    x += TWO_PI;

  /* Taylor series: sin(x) ≈ x - x^3/6 + x^5/120 */
  int x2 = fixmul(x, x);
  int x3 = fixmul(x2, x);
  int x5 = fixmul(x3, x2);

  int term1 = x;
  int term2 = fixmul(x3, 10923); /* x^3 / 6 */
  int term3 = fixmul(x5, 546);   /* x^5 / 120 */

  return term1 - term2 + term3;
}

/* User-space fixed-point Cosine (16.16) */
int cos_fp(int x) {
  /* cos(x) = sin(x + PI/2) */
  return sin_fp(x + 102944);
}

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
