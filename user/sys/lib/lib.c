/*
 * user/sys/lib/lib.c
 * Userland C runtime and system call wrapper library
 *
 * This file is the sole C runtime for all userland processes.  It is
 * compiled into lib.o and linked into every ELF (there is no shared library
 * mechanism).  It provides:
 *
 *   - Thin C wrappers around every _sys_*() assembly stub from syscall.S.
 *   - Standard I/O emulation (fopen/fclose/fread/fwrite/fseek/ftell) backed
 *     by file_read/file_write syscalls.
 *   - Formatting (printf, snprintf, sprintf, vsnprintf, vsscanf, sscanf).
 *   - Input event decoding (input_poll_event: keyboard and mouse IPC msgs).
 *   - Graphics helpers (graphics_draw_rect, graphics_blit, graphics_draw_text,
 *     graphics_load_image).
 *   - Partial POSIX-like shims (strdup, strtol, abs, fabs, atof, getenv,
 *     mkdir, system, stat, puts, fflush, remove, rename, vfprintf).
 *   - UTF-8 decoder (utf8_decode).
 *   - Stack smash protector stub (__stack_chk_guard, __stack_chk_fail).
 *
 * STB libraries (NOTE USR-BLOAT-01/02):
 *   STB_EASY_FONT_IMPLEMENTATION and STB_IMAGE_IMPLEMENTATION are compiled
 *   unconditionally here, adding ~52KB of text to lib.o.  With no
 *   --gc-sections in the link step every ELF (including 9-line crash.c)
 *   carries the full JPEG/PNG/GIF/BMP decoder stack (~500KB ELF total, 70%
 *   of which is DWARF debug data from -g).
 *
 * Kernel source inclusion (NOTE USR-LIB-01):
 *   vsnprintf.c, math.c, string.c are sourced directly from kernel/lib/ via
 *   relative #include paths.  Any internal change to those kernel files
 *   silently changes userland behaviour.
 *
 * Known issues:
 *   USR-LIB-01  (W2 BAD-IMPL) Directly #includes kernel/lib C sources;
 *               breaks the userland/kernel boundary.
 *   USR-LIB-02  (W2 BAD-IMPL) fclose() guards against NULL with
 *               `(size_t)fp > 10`, a fragile magic-value check.
 *   USR-LIB-03  (W1 BAD-IMPL) graphics_draw_text declares a 100KB static
 *               buffer, inflating .bss in every linked binary.
 *   USR-LIB-04  (W1 STUB) mkdir/system/getenv are no-ops; atof truncates
 *               decimal fractions via (double)atoi().
 *   USR-LIB-05  (W1 DOC) vfprintf ignores the stream arg and always writes
 *               to fd 1; stderr goes to stdout silently.
 *   USR-SEC-01  (W3 SECURITY) registry_read/write have no authentication;
 *               any process can overwrite any key.
 *   USR-SEC-02  (W3 SECURITY) send()/kill_process() accept arbitrary PIDs
 *               with no capability check.
 *   USR-BLOAT-01 (W2 BAD-IMPL·PERF) STB libs always compiled in; no gc-sections.
 *   USR-BLOAT-02 (W2 BAD-IMPL) -g DWARF retained in every ELF; not stripped.
 */
#include <os1.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <input.h>
#include <graphics.h>

/*
 * STB_EASY_FONT_IMPLEMENTATION: embed the stb_easy_font rasterizer.
 * NOTE(USR-BLOAT-01): Compiled unconditionally; linked into every ELF even
 * when no text rendering is used.  Suppressed warnings are normal for STB
 * single-header libs (unused static functions, missing prototypes).
 */
#define STB_EASY_FONT_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-braces"
#include <stb_easy_font.h>
/*
 * STB_IMAGE_IMPLEMENTATION: embed the full stb_image decoder.
 * STBI_NO_STDIO/LINEAR/HDR disable file-I/O and HDR format support that
 * are unavailable or unnecessary in a freestanding environment.
 * NOTE(USR-BLOAT-01): Adds ~50KB of .text to every ELF regardless of use.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stb_image.h>
#pragma GCC diagnostic pop

/* errno: global error variable expected by POSIX-style libc callers.
 * Not set by any syscall wrapper currently; placeholder only. */
int errno = 0;

/* --- Syscall Wrappers ---
 * Each function below is a thin C-callable veneer over an assembly stub in
 * user/arch/<arch>/syscall.S.  Arguments are passed in the arch ABI registers
 * (x0-x5 on AArch64, rdi/rsi/rdx/r10/r8/r9 on x86-64) by the C compiler;
 * the stub moves the syscall number into x8/rax and issues svc/syscall.
 *
 * NOTE(USR-SEC-02): send(), kill_process(), and spawn() accept arbitrary PIDs
 * and paths with no capability check; any process has full authority.
 */
long read(int fd, char *buf, unsigned long count) { return _sys_read(fd, buf, count); }
void write(int fd, const char *buf, size_t count) { _sys_write(fd, buf, count); }
long get_time(void) { return _sys_get_time(); }
int get_pid(void) { return _sys_get_pid(); }
/* exit: the while(1) after _sys_exit() is unreachable dead code that silences
 * the "noreturn" warning in compilers that do not see svc #0 as a terminator. */
void exit(int status) { _sys_exit(status); while(1); }
int spawn(const char *path) { return _sys_spawn(path); }
/* spawn_caps: explicit capability mask; spawn_level: the level's default
 * preset (request CAP_ALL and let the kernel clamp to the level ceiling). */
long spawn_caps(const char *path, int level, unsigned long caps) { return _sys_spawn_caps(path, level, caps); }
long spawn_level(const char *path, int level) { return _sys_spawn_caps(path, level, CAP_ALL); }
int kill_process(int pid) { return _sys_kill(pid); }
/* wait: maps to process_wait() in the kernel, which is NON-BLOCKING:
 * returns -1 if the process is alive, pid if reaped, -2 if not found. */
int wait(int pid) { return _sys_wait(pid); }
void draw(int x, int y, int w, int h, int color) { _sys_draw(x, y, w, h, color); }
void flush(void) { _sys_flush(); }
int create_window(int x, int y, int w, int h, const char *title) { return _sys_create_window(x, y, w, h, title); }
void destroy_window(int win_id) { _sys_destroy_window(win_id); }
void window_draw(int win_id, int x, int y, int w, int h, unsigned int color) { _sys_window_draw(win_id, x, y, w, h, color); }
void window_blit(int win_id, int x, int y, int w, int h, const unsigned int *buf) { _sys_window_blit(win_id, x, y, w, h, buf); }
void yield(void) { _sys_yield(); }
/* sleep: busy-waits by polling get_time() in a yield loop.
 * 'ticks' is in jiffies (100 Hz on the reference timer -> 1 tick ≈ 10 ms). */
void sleep(int ticks) { long end = get_time() + ticks; while (get_time() < end) yield(); }
void compositor_render(void) { _sys_compositor_render(); }
/* send/recv: IPC syscalls; pid==-1 means "any sender" in recv/try_recv. */
int send(int pid, struct ipc_message *msg) { return _sys_send(pid, msg); }
int recv(int pid, struct ipc_message *msg) { return _sys_recv(pid, msg); }
/* try_recv: non-blocking variant of recv (syscall #32); returns <0 if no
 * message is waiting, 0 on success.  Forward-declared here because the arch
 * syscall.S may not have a .global for it in the dead user/sys/lib/syscall.S. */
int try_recv(int pid, struct ipc_message *msg) { extern int _sys_try_recv(int pid, void *msg); return _sys_try_recv(pid, msg); }
void set_window_flags(int win_id, int flags) { _sys_window_set_flags(win_id, flags); }
void set_focus(int pid) { extern void _sys_set_focus(int pid); _sys_set_focus(pid); }

/* --- Shared Implementations (from kernel library) ---
 * NOTE(USR-LIB-01): These are direct source-level #includes of kernel
 * internal implementation files, not headers.  Changes to kernel/lib C files
 * silently affect userland behaviour with no compile-time boundary check.
 * vsnprintf.c provides vsnprintf/vsscanf; math.c provides fixed-point trig
 * and DEG_TO_FP_RAD/cos_fp/sin_fp/fixmul used by demo3d; string.c provides
 * memset/memcpy/strlen/strcmp/strncmp/strchr etc. */
#include "../../kernel/lib/vsnprintf.c"
#include "../../kernel/lib/math.c"
#include "../../kernel/lib/string.c"
#include "font_lib.c"

/* --- Stack protector support ---
 * __stack_chk_guard: canary value written by the compiler before local arrays
 * on functions compiled with -fstack-protector.  The value is a fixed constant
 * rather than a runtime random seed, weakening its effectiveness against local
 * attacks, but it is sufficient for a debug/development build.
 * __stack_chk_fail: called when the canary is clobbered; prints a message and
 * exits.  Must not call any function that itself uses stack protectors to avoid
 * infinite recursion. */
uintptr_t __stack_chk_guard = 0x595e9eda;
void __stack_chk_fail(void) { printf("Stack smashing detected!\n"); exit(1); }

/* --- Registry Wrappers ---
 * op=0 (SYS_REGISTRY): read value for 'key' into buf (size bytes).
 * op=1 (SYS_REGISTRY): write value for 'key'; size = strlen(value).
 *
 * NOTE(USR-SEC-01): No authentication; any process can read or overwrite any
 * key.  In particular, overwriting "srv.notify_pid" hijacks all notifications.
 */
int registry_read(const char *key, char *buf, size_t size) { return (int)_sys_registry(0, key, buf, size); }
int registry_write(const char *key, const char *value) { return (int)_sys_registry(1, key, (char *)value, strlen(value)); }

/*
 * set_font - transfer a packed font buffer to the kernel (SYS_SET_FONT #253).
 *
 * data: pointer to a [ font_header ][ glyph_info * n ][ bitmap ] buffer.
 * size: total byte count of that buffer.
 *
 * NOTE(USR-FONTMAN-01): The kernel stores 'data' as a raw pointer; the caller
 * must keep the buffer alive indefinitely (fontman uses while(1) yield()).
 */
int set_font(void *data, size_t size) {
  extern int _sys_set_font(void *data, size_t size);
  return _sys_set_font(data, size);
}
/* file_read: buf==NULL / size==0 returns the file size without reading data;
 * used by fopen() to probe file size before allocating a read buffer. */
int file_write(const char *path, const void *buf, int size, int offset) { return _sys_file_write(path, buf, size, offset); }
int file_read(const char *path, void *buf, int size, int offset) { return _sys_file_read(path, buf, size, offset); }
int list_dir(const char *path, char *buf, size_t size) { return _sys_list_dir(path, buf, size); }
int chdir(const char *path) { return _sys_chdir(path); }
int getcwd(char *buf, size_t size) { return _sys_getcwd(buf, size); }

/* POSIX-style fd I/O (ABI-03 fd table).  open() matches the variadic
 * declaration in fcntl.h; the optional mode argument is ignored because the
 * VFS cannot create files yet (the kernel rejects O_CREAT with -EINVAL). */
int open(const char *pathname, int flags, ...) { return _sys_open(pathname, flags); }
int close(int fd) { return _sys_close(fd); }
long lseek(int fd, long offset, int whence) { return _sys_lseek(fd, offset, whence); }

/* --- Formatting & Printing ---
 * All formatting functions delegate to vsnprintf() from kernel/lib/vsnprintf.c
 * (included above).  Output goes to fd 1 (the shell/window TTY) via write().
 *
 * printf: uses a 256-byte stack buffer; output longer than 255 chars is
 * silently truncated by vsnprintf.
 *
 * printf_win: like printf but writes to a compositor window's fd (win_id)
 * via _sys_write, which routes to the compositor terminal emulator.
 *
 * vsprintf/sprintf: pass 65536 as the size limit — effectively unbounded.
 * Callers are responsible for providing a large enough destination buffer;
 * overflow is not detected.
 *
 * print_hex: renders a 64-bit value as 18-char "0xHHHHHHHHHHHHHHHH" string
 * written directly via write(), bypassing the format engine.
 */
int vsprintf(char *out, const char *fmt, va_list args) { return vsnprintf(out, 65536, fmt, args); }
int printf(const char *fmt, ...) { char buf[256]; va_list args; va_start(args, fmt); int res = vsnprintf(buf, sizeof(buf), fmt, args); va_end(args); write(1, buf, strlen(buf)); return res; }
void printf_win(int win_id, const char *fmt, ...) { char buf[512]; va_list args; va_start(args, fmt); vsnprintf(buf, sizeof(buf), fmt, args); va_end(args); _sys_write(win_id, buf, strlen(buf)); }
int sprintf(char *out, const char *fmt, ...) { va_list args; va_start(args, fmt); int res = vsnprintf(out, 65536, fmt, args); va_end(args); return res; }
int snprintf(char *out, size_t size, const char *fmt, ...) { va_list args; va_start(args, fmt); int res = vsnprintf(out, size, fmt, args); va_end(args); return res; }
void print(const char *s) { write(1, s, strlen(s)); }
/* print_hex: manual 16-nibble hex formatter for a 64-bit value. */
void print_hex(unsigned long val) { char buf[18]; buf[0] = '0'; buf[1] = 'x'; for (int i = 0; i < 16; i++) { int digit = (val >> ((15 - i) * 4)) & 0xF; buf[2 + i] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10); } write(1, buf, 18); }

/* --- Standard IO ---
 * getchar: blocking single-char read from fd 0 (keyboard).
 * putchar: writes one character to fd 1 (TTY/window).
 * gets: line-buffered input with backspace handling; size-bounded to avoid
 *   overflow (stops at size-1 chars).  Echoes characters to fd 1 and handles
 *   \b/DEL with the terminal backspace-space-backspace sequence.
 */
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

/*
 * notify - send a system notification via IPC to the notification server.
 *
 * title: short label (up to 30 chars copied); truncated silently if longer.
 * msg:   message body (up to 33 remaining chars after "title: "); truncated.
 *
 * The payload is assembled as "title: msg\0" into imsg.payload[64].
 * The target PID is read from the global registry key "srv.notify_pid".
 * Falls back to PID 2 if the key is absent (pid=2 is the expected notify_srv
 * PID under the current fixed-order spawn sequence in init.c).
 *
 * NOTE(USR-SEC-01): registry_read("srv.notify_pid", ...) has no authentication;
 * any process can overwrite that key to redirect all notifications to itself,
 * effectively hijacking the system notification channel.
 *
 * Returns the result of send() (0 on success, negative on failure).
 */
int notify(const char *title, const char *msg) {
  struct ipc_message imsg;
  imsg.type = IPC_TYPE_NOTIFY;
  imsg.data1 = 0;
  imsg.data2 = 0;
  int i = 0;
  /* Pack "title: msg" into the 64-byte payload field.
   * 30-char limit for title leaves room for ": " and at least 32 msg chars. */
  while (*title && i < 30) imsg.payload[i++] = *title++;
  imsg.payload[i++] = ':'; imsg.payload[i++] = ' ';
  while (*msg && i < 63) imsg.payload[i++] = *msg++;
  imsg.payload[i] = '\0';
  char pid_buf[16];
  int pid = 2;  /* Default: notify_srv is typically the second process spawned */
  if (registry_read("srv.notify_pid", pid_buf, sizeof(pid_buf)) == 0) {
    pid = atoi(pid_buf);
  }
  return send(pid, &imsg);
}

/* --- Doom/LibC Compatibility ---
 * FILE emulation: a FILE* is a heap-allocated struct (defined in os1.h) that
 * records the file path, current byte position, error/eof flags, and cached
 * size.  All I/O is synchronous and unbuffered; every fread/fwrite call maps
 * directly to a file_read/file_write syscall with the current offset.
 */

/*
 * fopen - open a file for buffered I/O emulation.
 *
 * Allocates a FILE struct, stores the path, probes the file size via
 * file_read(path, NULL, 0, 0) (size-probe convention: buf==NULL returns size).
 * Returns NULL if the file does not exist and mode is "r" (read-only).
 * Write modes ("w", "a") do not fail on missing files — file_write will
 * create them on demand via the kernel VFS.
 */
FILE *fopen(const char *path, const char *mode) {
  FILE *f = malloc(sizeof(FILE));
  if (!f) return NULL;
  strncpy(f->path, path, sizeof(f->path) - 1);
  f->pos = 0;
  f->error = 0;
  f->eof = 0;
  /* Probe file size; file_read with NULL buf and size=0 returns byte count. */
  f->size = file_read(path, NULL, 0, 0);
  if (f->size < 0 && mode[0] == 'r') {
    free(f);
    return NULL;
  }
  return f;
}

/*
 * fclose - release a FILE handle.
 *
 * NOTE(USR-LIB-02): Guards against NULL with `(size_t)fp > 10`, which is
 * intended to catch sentinel/invalid values stored as small integers in some
 * callers (e.g. fileno tricks).  A proper NULL check (`fp != NULL`) would be
 * more idiomatic; this heuristic silently allows freeing invalid pointers with
 * addresses 1-10, which would corrupt the heap.
 */
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

/* --- Standard Input Library ---
 * Input events are delivered as IPC messages from the kernel input driver.
 * IPC_TYPE_INPUT carries keyboard data; IPC_TYPE_MOUSE carries mouse data.
 * Both are received non-blocking via try_recv(-1, ...) — poll any sender.
 */

/*
 * input_poll_event - check for and decode the next pending input event.
 *
 * event: output parameter filled on success.
 *
 * Returns 1 if an event was decoded (event is valid), 0 if no message was
 * waiting or the message type is not a recognised input type.
 *
 * IPC_TYPE_INPUT layout (data1/data2/payload):
 *   data1 low byte : ASCII key code (keyboard.key)
 *   data1 bits 16+ : HID scancode   (keyboard.scancode)
 *   data2           : key state (0=released, 1=pressed, 2=repeat)
 *   payload[0..7]  : UTF-8 encoded character (up to 4 bytes + NUL)
 *
 * IPC_TYPE_MOUSE layout:
 *   data1  : button mask
 *   data2  : button state (pressed=1)
 *   payload[0..3]  : x coordinate (int32, little-endian)
 *   payload[4..7]  : y coordinate (int32, little-endian)
 *
 * Note: memcpy is used for mouse coordinates to handle potential alignment
 * constraints on the int fields within the packed payload array.
 */
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

/* --- Graphics Library ---
 * High-level drawing wrappers that delegate to the compositor syscalls.
 */

/*
 * graphics_draw_rect - fill a rectangle in a compositor window.
 *
 * Thin wrapper over window_draw() -> SYS_WINDOW_DRAW (#211).
 * color is ARGB (0xAARRGGBB).
 */
void graphics_draw_rect(int win_id, int x, int y, int w, int h, uint32_t color) {
  window_draw(win_id, x, y, w, h, color);
}

/*
 * graphics_blit - upload a pixel buffer to a compositor window region.
 *
 * buffer must be w*h uint32_t pixels in ARGB row-major order.
 * Delegates to window_blit() -> SYS_WINDOW_BLIT (#213).
 */
void graphics_blit(int win_id, int x, int y, int w, int h, const uint32_t *buffer) {
  window_blit(win_id, x, y, w, h, buffer);
}

/*
 * graphics_draw_text - render text into a compositor window.
 *
 * NOTE(USR-LIB-03): Declares `static char buffer[99999]` — a 100KB static
 * BSS allocation present in every binary that links lib.o, regardless of
 * whether this function is ever called (no --gc-sections).
 *
 * The stb_easy_font geometry is computed but discarded; rendering falls back
 * to _sys_write (the compositor terminal emulator font) because the quad-to-
 * pixel blitting loop is not yet implemented.  The x, y, and color arguments
 * are therefore ignored (marked void to suppress warnings).
 *
 * Returns an estimate of the rendered width (strlen * 8 pixels), which is
 * inaccurate for proportional fonts.
 */
int graphics_draw_text(int win_id, int x, int y, const char *text, uint32_t color) {
  (void)color; (void)x; (void)y;
  static char buffer[99999]; /* NOTE(USR-LIB-03): 100KB static buffer in .bss */
  int num_quads = stb_easy_font_print(0, 0, (char*)text, NULL, buffer, sizeof(buffer));
  (void)num_quads;

  /* stb_easy_font returns quads, but the quad-to-pixel rendering loop is not
   * implemented.  Fall back to the compositor's built-in terminal font via
   * _sys_write, losing x/y positioning and color control. */
  _sys_write(win_id, text, strlen(text)); /* Uses compositor terminal emulator */
  return strlen(text) * 8;
}

/*
 * graphics_load_image - load an image file and decode it to ARGB pixels.
 *
 * path: filesystem path to a JPEG, PNG, GIF, or BMP file.
 * w, h: output parameters for image dimensions.
 *
 * Uses file_read with buf==NULL to probe the file size, then reads the full
 * file into a temporary heap buffer and decodes with stbi_load_from_memory()
 * requesting 4 channels (RGBA -> reinterpreted as ARGB32).  The raw file
 * buffer is freed immediately; the decoded pixel buffer from stbi is returned
 * to the caller, who owns it (must free() eventually).
 *
 * Returns NULL on file-not-found, malloc failure, or decode error.
 */
uint32_t *graphics_load_image(const char *path, int *w, int *h) {
  int size = file_read(path, NULL, 0, 0);  /* Probe file size */
  if (size <= 0) return NULL;
  unsigned char *data = malloc(size);
  if (!data) return NULL;
  if (file_read(path, data, size, 0) != size) {
    free(data);
    return NULL;
  }
  int n;  /* Actual channel count from file; forced to 4 by the last arg */
  unsigned char *img = stbi_load_from_memory(data, size, w, h, &n, 4);
  free(data);  /* Release raw file buffer; decoded buffer is returned */
  return (uint32_t *)img;
}

/*
 * strtol - convert string to long integer with base and endptr support.
 *
 * Handles leading whitespace, optional sign, 0x/0 prefixes for base
 * auto-detection (base==0), and digits up to the given base.
 * Sets *endptr to the first non-consumed character if endptr != NULL.
 * Does not detect overflow (val accumulates without range check).
 * Negative values are produced by negating the unsigned accumulator,
 * which gives correct two's-complement representation for LONG_MIN.
 */
long strtol(const char *nptr, char **endptr, int base) {
  const char *p = nptr;
  while (isspace(*p)) p++;
  int neg = 0;
  if (*p == '-') { neg = 1; p++; }
  else if (*p == '+') p++;

  /* Auto-detect base from prefix: "0x" -> 16, "0" -> 8, else 10. */
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

/*
 * sscanf - varargs wrapper that delegates to vsscanf.
 */
int sscanf(const char *str, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int res = vsscanf(str, format, args);
  va_end(args);
  return res;
}

/*
 * vsscanf - simplified format scanner supporting %d, %x/%X, %s with widths.
 *
 * Supports:
 *   %d  - signed decimal integer -> int *
 *   %x, %X - unsigned hex integer -> unsigned int *; skips optional 0x prefix
 *   %s  - whitespace-delimited string; width limits chars consumed
 *
 * Whitespace in the format string matches zero or more whitespace chars in
 * the input.  Literal format characters must match exactly (returns early on
 * mismatch).  Returns the count of successfully assigned conversions.
 *
 * Missing specifiers: %f, %c, %ld, %u, %p, etc.  Callers using unsupported
 * format specifiers will silently skip the conversion.
 */
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
    fmt++; /* skip % */
    /* Parse optional field width */
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
    /* NOTE(USR-LIB-04): Other format specifiers (%f, %c, %ld, %u, etc.) are
     * silently skipped; the corresponding va_arg is NOT consumed, which may
     * desync the va_list for subsequent conversions. */
  }
  return nassigned;
}

/* NOTE(USR-LIB-04): The following four functions are stubs.
 * mkdir/system/getenv return no-op values; atof truncates decimal fractions
 * by delegating to atoi() and casting.  Callers expecting correct behaviour
 * (e.g. a floating-point string "3.14" -> 3.14) silently receive 3.0. */
int mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }
int system(const char *command) { (void)command; return 0; }
/* atof: NOTE(USR-LIB-04) only integer part is parsed; decimal digits ignored. */
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

/*
 * vfprintf - format and write to a FILE stream.
 *
 * NOTE(USR-LIB-05): The 'stream' argument is ignored; output always goes to
 * fd 1 (stdout/TTY).  Any code that writes to stderr (e.g. fprintf(stderr, ...))
 * will silently produce output on stdout instead of the error channel.
 *
 * Output is limited to 1023 chars by the stack buffer; longer output is
 * silently truncated.  Always returns 0 (not the character count).
 */
int vfprintf(FILE *stream, const char *format, va_list ap) {
  (void)stream;  /* NOTE(USR-LIB-05): stream ignored; always writes to fd 1 */
  char buf[1024];
  vsnprintf(buf, sizeof(buf), format, ap);
  write(1, buf, strlen(buf));
  return 0;
}
/* fflush: no-op (no userland buffer to flush; writes are unbuffered). */
int fflush(FILE *stream) { (void)stream; return 0; }
/* remove/rename: stubs returning success; no VFS deletion/rename syscall yet. */
int remove(const char *pathname) { (void)pathname; return 0; }
int rename(const char *oldpath, const char *newpath) { (void)oldpath; (void)newpath; return 0; }
/* puts: writes string + newline to fd 1, matching the standard POSIX contract. */
int puts(const char *s) { write(1, s, strlen(s)); write(1, "\n", 1); return 0; }

/*
 * utf8_decode - decode the first UTF-8 codepoint from string s.
 *
 * s:    pointer to the start of a UTF-8 byte sequence (not necessarily NUL).
 * code: output parameter; receives the Unicode codepoint on success.
 *
 * Returns the number of bytes consumed (1–4), or 0 on invalid/null input.
 *
 * Encoding rules applied:
 *   0xxxxxxx (< 0x80)      : 1-byte ASCII
 *   110xxxxx 10xxxxxx      : 2-byte (U+0080..U+07FF)
 *   1110xxxx 10xxxxxx x2   : 3-byte (U+0800..U+FFFF)
 *   11110xxx 10xxxxxx x3   : 4-byte (U+10000..U+10FFFF)
 *
 * No validation of continuation bytes (0x3F mask is applied without checking
 * the 0x80 bit); malformed sequences may produce incorrect codepoints silently.
 *
 * Used by font_lib.c:font_draw_string() to iterate a UTF-8 string glyph by glyph.
 */
int utf8_decode(const char *s, uint32_t *code) {
  if (!s || !code) return 0;
  unsigned char c = (unsigned char)s[0];

  if (c < 0x80) {
    /* Single-byte ASCII codepoint */
    *code = c;
    return 1;
  } else if ((c & 0xE0) == 0xC0) {
    /* 2-byte sequence: 110xxxxx 10xxxxxx */
    *code = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
    return 2;
  } else if ((c & 0xF0) == 0xE0) {
    /* 3-byte sequence: 1110xxxx 10xxxxxx 10xxxxxx */
    *code = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
    return 3;
  } else if ((c & 0xF8) == 0xF0) {
    /* 4-byte sequence: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
    *code = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
            ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
    return 4;
  }
  return 0;  /* Unrecognised lead byte (invalid UTF-8) */
}
