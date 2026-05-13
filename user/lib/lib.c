/*
 * user/lib.c
 * User-space library - delegates to syscall.S wrappers
 */
#include <os1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int errno = 0;

/* --- Syscall Wrappers --- */
long read(int fd, char *buf, unsigned long count) { return _sys_read(fd, buf, count); }
void write(int fd, const char *buf, size_t count) { _sys_write(fd, buf, count); }
long get_time(void) { return _sys_get_time(); }
int get_pid(void) { return _sys_get_pid(); }
void exit(int status) { _sys_exit(status); while(1); }
int spawn(const char *path) { return _sys_spawn(path); }
int kill_process(int pid) { return _sys_kill(pid); }
int wait(int pid) { return _sys_wait(pid); }
void draw(int x, int y, int w, int h, int color) { _sys_draw(x, y, w, h, color); }
void flush(void) { _sys_flush(); }
int create_window(int x, int y, int w, int h, const char *title) { return _sys_create_window(x, y, w, h, title); }
void destroy_window(int win_id) { _sys_destroy_window(win_id); }
void window_draw(int win_id, int x, int y, int w, int h, unsigned int color) { _sys_window_draw(win_id, x, y, w, h, color); }
void window_blit(int win_id, int x, int y, int w, int h, const unsigned int *buf) { _sys_window_blit(win_id, x, y, w, h, buf); }
void yield(void) { _sys_yield(); }
void sleep(int ticks) { long end = get_time() + ticks; while (get_time() < end) yield(); }
void compositor_render(void) { _sys_compositor_render(); }
int send(int pid, struct ipc_message *msg) { return _sys_send(pid, msg); }
int recv(int pid, struct ipc_message *msg) { return _sys_recv(pid, msg); }
int try_recv(int pid, struct ipc_message *msg) { extern int _sys_try_recv(int pid, void *msg); return _sys_try_recv(pid, msg); }
void set_window_flags(int win_id, int flags) { _sys_window_set_flags(win_id, flags); }
void set_focus(int pid) { extern void _sys_set_focus(int pid); _sys_set_focus(pid); }

/* --- Shared Implementations (from kernel library) --- */
#include "../../kernel/lib/vsnprintf.c"
#include "../../kernel/lib/math.c"
#include "../../kernel/lib/string.c"

/* --- Stack protector support --- */
uintptr_t __stack_chk_guard = 0x595e9eda;
void __stack_chk_fail(void) { printf("Stack smashing detected!\n"); exit(1); }

/* --- Registry Wrappers --- */
int registry_read(const char *key, char *buf, size_t size) { return (int)_sys_registry(0, key, buf, size); }
int registry_write(const char *key, const char *value) { return (int)_sys_registry(1, key, (char *)value, 0); }
int file_write(const char *path, const void *buf, int size, int offset) { return _sys_file_write(path, buf, size, offset); }
int file_read(const char *path, void *buf, int size, int offset) { return _sys_file_read(path, buf, size, offset); }

/* --- Formatting & Printing --- */
int vsprintf(char *out, const char *fmt, va_list args) { return vsnprintf(out, 65536, fmt, args); }
int printf(const char *fmt, ...) { char buf[256]; va_list args; va_start(args, fmt); int res = vsnprintf(buf, sizeof(buf), fmt, args); va_end(args); write(1, buf, strlen(buf)); return res; }
void printf_win(int win_id, const char *fmt, ...) { char buf[512]; va_list args; va_start(args, fmt); vsnprintf(buf, sizeof(buf), fmt, args); va_end(args); _sys_write(win_id, buf, strlen(buf)); }
int sprintf(char *out, const char *fmt, ...) { va_list args; va_start(args, fmt); int res = vsnprintf(out, 65536, fmt, args); va_end(args); return res; }
int snprintf(char *out, size_t size, const char *fmt, ...) { va_list args; va_start(args, fmt); int res = vsnprintf(out, size, fmt, args); va_end(args); return res; }
void print(const char *s) { write(1, s, strlen(s)); }
void print_hex(unsigned long val) { char buf[18]; buf[0] = '0'; buf[1] = 'x'; for (int i = 0; i < 16; i++) { int digit = (val >> ((15 - i) * 4)) & 0xF; buf[2 + i] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10); } write(1, buf, 18); }

/* --- Standard IO --- */
int getchar(void) { char c; if (read(0, &c, 1) == 1) return (unsigned char)c; return -1; }
int putchar(int c) { char ch = (char)c; write(1, &ch, 1); return c; }
char *gets(char *s, int size) {
    int i = 0;
    while (i < size - 1) {
        int c = getchar();
        if (c < 0) break;
        if (c == '\b' || c == 127) { if (i > 0) { i--; write(1, "\b \b", 3); } continue; }
        putchar(c);
        if (c == '\n' || c == '\r') { s[i] = '\0'; return s; }
        s[i++] = (char)c;
    }
    s[i] = '\0';
    return s;
}

int notify(const char *title, const char *msg) {
  struct ipc_message imsg;
  imsg.type = IPC_TYPE_NOTIFY;
  imsg.data1 = 0;
  imsg.data2 = 0;
  int i = 0;
  while (*title && i < 30) imsg.payload[i++] = *title++;
  imsg.payload[i++] = ':'; imsg.payload[i++] = ' ';
  while (*msg && i < 63) imsg.payload[i++] = *msg++;
  imsg.payload[i] = '\0';
  char pid_buf[16];
  int pid = 2;
  if (registry_read("srv.notify_pid", pid_buf, sizeof(pid_buf)) == 0) {
    pid = atoi(pid_buf);
  }
  return send(pid, &imsg);
}

/* --- Doom/LibC Compatibility --- */

FILE *fopen(const char *path, const char *mode) {
  FILE *f = malloc(sizeof(FILE));
  if (!f) return NULL;
  strncpy(f->path, path, sizeof(f->path) - 1);
  f->pos = 0;
  f->error = 0;
  f->eof = 0;
  // Get file size
  f->size = file_read(path, NULL, 0, 0); 
  if (f->size < 0 && mode[0] == 'r') {
    free(f);
    return NULL;
  }
  return f;
}

int fclose(FILE *fp) {
  if (fp && (size_t)fp > 10) free(fp);
  return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
  if (!fp || (size_t)fp <= 10) return 0;
  int bytes = size * nmemb;
  int read_bytes = file_read(fp->path, ptr, bytes, fp->pos);
  if (read_bytes < 0) {
    fp->error = 1;
    return 0;
  }
  fp->pos += read_bytes;
  if (read_bytes < bytes) fp->eof = 1;
  return read_bytes / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
  if (!fp || (size_t)fp <= 10) return 0;
  int bytes = size * nmemb;
  int written = file_write(fp->path, ptr, bytes, fp->pos);
  if (written < 0) {
    fp->error = 1;
    return 0;
  }
  fp->pos += written;
  return written / size;
}

int fseek(FILE *fp, long offset, int whence) {
  if (!fp || (size_t)fp <= 10) return -1;
  if (whence == SEEK_SET) fp->pos = offset;
  else if (whence == SEEK_CUR) fp->pos += offset;
  else if (whence == SEEK_END) {
    if (fp->size < 0) fp->size = file_read(fp->path, NULL, 0, 0);
    fp->pos = fp->size + offset;
  }
  if (fp->pos < 0) fp->pos = 0;
  fp->eof = 0;
  return 0;
}

long ftell(FILE *fp) {
  if (!fp || (size_t)fp <= 10) return -1;
  return fp->pos;
}

int feof(FILE *fp) { return fp ? fp->eof : 1; }
int ferror(FILE *fp) { return fp ? fp->error : 1; }

char *strdup(const char *s) {
  size_t len = strlen(s) + 1;
  char *res = malloc(len);
  if (res) memcpy(res, s, len);
  return res;
}

int abs(int j) { return (j < 0) ? -j : j; }

int sscanf(const char *str, const char *format, ...) {
  // Ultra-minimal sscanf for Doom's M_StrToInt
  va_list args;
  va_start(args, format);
  int count = 0;
  if (strstr(format, "%x") || strstr(format, "%X")) {
    int *res = va_arg(args, int *);
    const char *p = str;
    while (*p == ' ') p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    unsigned int val = 0;
    while (1) {
      char c = *p++;
      if (c >= '0' && c <= '9') val = (val << 4) | (c - '0');
      else if (c >= 'a' && c <= 'f') val = (val << 4) | (c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') val = (val << 4) | (c - 'A' + 10);
      else break;
    }
    *res = (int)val;
    count = 1;
  } else if (strstr(format, "%d")) {
    int *res = va_arg(args, int *);
    *res = atoi(str);
    count = 1;
    va_end(args);
  }
  return count;
}

int mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }
int system(const char *command) { (void)command; return 0; }
double atof(const char *nptr) { return (double)atoi(nptr); }
char *getenv(const char *name) { (void)name; return NULL; }
int stat(const char *path, struct stat *buf) {
  if (buf) memset(buf, 0, sizeof(struct stat));
  int size = file_read(path, NULL, 0, 0);
  if (size < 0) return -1;
  if (buf) {
    buf->st_size = size;
    buf->st_mode = S_IFREG;
  }
  return 0;
}

int vfprintf(FILE *stream, const char *format, va_list ap) {
  (void)stream;
  char buf[1024];
  vsnprintf(buf, sizeof(buf), format, ap);
  write(1, buf, strlen(buf));
  return 0;
}
int fflush(FILE *stream) { (void)stream; return 0; }
int remove(const char *pathname) { (void)pathname; return 0; }
int rename(const char *oldpath, const char *newpath) { (void)oldpath; (void)newpath; return 0; }
int puts(const char *s) { write(1, s, strlen(s)); write(1, "\n", 1); return 0; }
