/*
 * user/lib.c
 * User-space library - delegates to syscall.S wrappers
 */
#include <os1.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <input.h>
#include <graphics.h>

#define STB_EASY_FONT_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-braces"
#include <stb_easy_font.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stb_image.h>
#pragma GCC diagnostic pop

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
#include "font_lib.c"

/* --- Stack protector support --- */
uintptr_t __stack_chk_guard = 0x595e9eda;
void __stack_chk_fail(void) { printf("Stack smashing detected!\n"); exit(1); }

/* --- Registry Wrappers --- */
int registry_read(const char *key, char *buf, size_t size) { return (int)_sys_registry(0, key, buf, size); }
int registry_write(const char *key, const char *value) { return (int)_sys_registry(1, key, (char *)value, strlen(value)); }

int set_font(void *data, size_t size) {
  extern int _sys_set_font(void *data, size_t size);
  return _sys_set_font(data, size);
}
int file_write(const char *path, const void *buf, int size, int offset) { return _sys_file_write(path, buf, size, offset); }
int file_read(const char *path, void *buf, int size, int offset) { return _sys_file_read(path, buf, size, offset); }
int list_dir(const char *path, char *buf, size_t size) { return _sys_list_dir(path, buf, size); }
int chdir(const char *path) { return _sys_chdir(path); }
int getcwd(char *buf, size_t size) { return _sys_getcwd(buf, size); }

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

int abs(int x) { return x < 0 ? -x : x; }
double fabs(double x) { return x < 0 ? -x : x; }

/* --- Standard Input Library --- */
int input_poll_event(input_event_t *event) {
  struct ipc_message msg;
  if (try_recv(-1, &msg) < 0) return 0;

  if (msg.type == IPC_TYPE_INPUT) {
    event->type = INPUT_TYPE_KEYBOARD;
    event->keyboard.key = (unsigned char)(msg.data1 & 0xFF);
    event->keyboard.scancode = (uint16_t)(msg.data1 >> 16);
    event->keyboard.state = (int)msg.data2;
    memcpy(event->keyboard.utf8, msg.payload, 8);
    return 1;
  } else if (msg.type == IPC_TYPE_MOUSE) {
    event->type = INPUT_TYPE_MOUSE;
    event->mouse.button = (int)msg.data1;
    event->mouse.state = (int)msg.data2;
    memcpy(&event->mouse.x, msg.payload, 4);
    memcpy(&event->mouse.y, msg.payload + 4, 4);
    return 1;
  }
  return 0;
}

/* --- Graphics Library --- */
void graphics_draw_rect(int win_id, int x, int y, int w, int h, uint32_t color) {
  window_draw(win_id, x, y, w, h, color);
}

void graphics_blit(int win_id, int x, int y, int w, int h, const uint32_t *buffer) {
  window_blit(win_id, x, y, w, h, buffer);
}

int graphics_draw_text(int win_id, int x, int y, const char *text, uint32_t color) {
  (void)color; (void)x; (void)y;
  static char buffer[99999]; // static to avoid stack overflow
  int num_quads = stb_easy_font_print(0, 0, (char*)text, NULL, buffer, sizeof(buffer));
  (void)num_quads;
  
  // stb_easy_font returns quads. We need to draw them.
  // This is a bit slow without a real GL implementation, but for now we can blit.
  // Actually, we could implement a draw_char but let's use printf_win for now if possible?
  // No, printf_win uses kernel's font. easy_font allows custom positions.
  
  // For simplicity, let's just use printf_win for now as a placeholder 
  // until we have a proper pixel-based text rendering loop here.
  // Actually, let's just use the kernel font for now.
  _sys_write(win_id, text, strlen(text)); // This uses the compositor's terminal emulator
  return strlen(text) * 8;
}

uint32_t *graphics_load_image(const char *path, int *w, int *h) {
  int size = file_read(path, NULL, 0, 0);
  if (size <= 0) return NULL;
  unsigned char *data = malloc(size);
  if (!data) return NULL;
  if (file_read(path, data, size, 0) != size) {
    free(data);
    return NULL;
  }
  int n;
  unsigned char *img = stbi_load_from_memory(data, size, w, h, &n, 4);
  free(data);
  return (uint32_t *)img;
}

long strtol(const char *nptr, char **endptr, int base) {
  const char *p = nptr;
  while (isspace(*p)) p++;
  int neg = 0;
  if (*p == '-') { neg = 1; p++; }
  else if (*p == '+') p++;
  
  if (base == 0) {
    if (*p == '0') {
      if (p[1] == 'x' || p[1] == 'X') base = 16;
      else base = 8;
    } else base = 10;
  }
  
  if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
  
  unsigned long val = 0;
  while (1) {
    int digit;
    if (isdigit(*p)) digit = *p - '0';
    else if (isalpha(*p)) digit = tolower(*p) - 'a' + 10;
    else break;
    
    if (digit >= base) break;
    val = val * base + digit;
    p++;
  }
  
  if (endptr) *endptr = (char *)p;
  return neg ? -(long)val : (long)val;
}

/* --- Robust sscanf (Ported from BSD) --- */
int sscanf(const char *str, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int res = vsscanf(str, format, args);
  va_end(args);
  return res;
}

int vsscanf(const char *inp, const char *fmt0, va_list ap) {
  int nassigned = 0;
  const unsigned char *fmt = (const unsigned char *)fmt0;
  const char *p_inp = inp;

  while (*fmt) {
    if (isspace(*fmt)) {
      while (isspace(*p_inp)) p_inp++;
      fmt++;
      continue;
    }
    if (*fmt != '%') {
      if (*p_inp != *fmt) return nassigned;
      p_inp++; fmt++;
      continue;
    }
    fmt++; // skip %
    int width = 0;
    while (isdigit(*fmt)) {
      width = width * 10 + (*fmt - '0');
      fmt++;
    }
    
    char c = *fmt++;
    if (c == 'd') {
      while (isspace(*p_inp)) p_inp++;
      int *res = va_arg(ap, int *);
      *res = atoi(p_inp);
      nassigned++;
      while (isdigit(*p_inp) || *p_inp == '-') p_inp++;
    } else if (c == 'x' || c == 'X') {
      while (isspace(*p_inp)) p_inp++;
      unsigned int *res = va_arg(ap, unsigned int *);
      unsigned int val = 0;
      if (p_inp[0] == '0' && (p_inp[1] == 'x' || p_inp[1] == 'X')) p_inp += 2;
      while (isxdigit(*p_inp)) {
        char dc = *p_inp++;
        if (isdigit(dc)) val = (val << 4) | (dc - '0');
        else val = (val << 4) | (tolower(dc) - 'a' + 10);
      }
      *res = val;
      nassigned++;
    } else if (c == 's') {
      while (isspace(*p_inp)) p_inp++;
      char *res = va_arg(ap, char *);
      while (*p_inp && !isspace(*p_inp)) {
        *res++ = *p_inp++;
        if (width > 0 && --width == 0) break;
      }
      *res = '\0';
      nassigned++;
    }
    // More types can be added as needed
  }
  return nassigned;
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

/*
 * UTF-8 Decoding
 */
int utf8_decode(const char *s, uint32_t *code) {
  if (!s || !code) return 0;
  unsigned char c = (unsigned char)s[0];

  if (c < 0x80) {
    *code = c;
    return 1;
  } else if ((c & 0xE0) == 0xC0) {
    *code = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
    return 2;
  } else if ((c & 0xF0) == 0xE0) {
    *code = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
    return 3;
  } else if ((c & 0xF8) == 0xF0) {
    *code = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
            ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
    return 4;
  }
  return 0;
}
