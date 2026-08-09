#ifndef _STDIO_H
#define _STDIO_H

#include <os1.h>
#include <stdarg.h>

/* FILE_WBUF_SIZE: userland write-buffer capacity for positional (path-backed)
 * streams.  Sized to the ext4 block so buffered flushes land block-aligned and
 * a per-byte fwrite loop (e.g. a savegame written field-by-field) coalesces
 * into one syscall per 4 KiB instead of one per call. */
#define FILE_WBUF_SIZE 4096
#define FILE_RBUF_SIZE 4096

typedef struct {
    int fd;
    int pos;
    int size;
    int error;
    int eof;
    char path[128];
    int ungetc_buf;
    int has_ungetc;
    int is_tmp;
    /* Write buffer for positional streams (fd < 0).  'wcount' bytes are pending;
     * their on-disk offset is (pos - wcount).  Empty (wcount == 0) for read
     * streams and the console std streams, which stay unbuffered. */
    int wcount;
    char wbuf[FILE_WBUF_SIZE];
    /* READ buffer, symmetric with the write one and for the same reason: the
     * filesystem reads a whole 4 KiB block for any partial read, so an
     * unbuffered byte-at-a-time reader re-reads the same block once per byte.
     * rhead is the consumed offset inside rbuf, rcount the valid bytes; rbase
     * is the file offset rbuf[0] came from, so a seek can tell whether the
     * cached window still covers the new position. */
    int rcount;
    int rhead;
    int rbase;
    char rbuf[FILE_RBUF_SIZE];
} FILE;

extern FILE _stdin_struct;
extern FILE _stdout_struct;
extern FILE _stderr_struct;

#define stdin  (&_stdin_struct)
#define stdout (&_stdout_struct)
#define stderr (&_stderr_struct)

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#ifndef BUFSIZ
#define BUFSIZ 1024
#endif

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#ifndef EOF
#define EOF (-1)
#endif

FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *filename, const char *mode, FILE *stream);
FILE *fdopen(int fd, const char *mode);
int fclose(FILE *fp);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int fseek(FILE *fp, long offset, int whence);
long ftell(FILE *fp);
int feof(FILE *fp);
int ferror(FILE *fp);
int fflush(FILE *stream);
int remove(const char *pathname);
int rename(const char *oldpath, const char *newpath);

/* NX_PRINTFLIKE - tell the compiler these take a printf format string, so
 * -Wformat (already on via -Wall -Werror) actually CHECKS the arguments.
 *
 * Without this the checking is silently absent: a `%s` fed an int compiles
 * clean and then dereferences that integer as a pointer at runtime.  That
 * exact bug crashed the dock on every frame, and because init has no respawn
 * rate limiting it became a boot-killing loop and finally a kernel panic — from
 * a mismatch the compiler was fully capable of catching. */
#define NX_PRINTFLIKE(fmt_idx, first_arg)                                      \
  __attribute__((format(printf, fmt_idx, first_arg)))

int printf(const char *format, ...) NX_PRINTFLIKE(1, 2);
int sprintf(char *str, const char *format, ...) NX_PRINTFLIKE(2, 3);
int snprintf(char *str, size_t size, const char *format, ...)
    NX_PRINTFLIKE(3, 4);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int vsprintf(char *str, const char *format, va_list ap);
int vfprintf(FILE *stream, const char *format, va_list ap);
int sscanf(const char *str, const char *format, ...);
int vsscanf(const char *str, const char *format, va_list ap);

int puts(const char *s);
int putchar(int c);
int getchar(void);

int fputc(int c, FILE *fp);
int fputs(const char *s, FILE *fp);
int fgetc(FILE *fp);
char *fgets(char *s, int size, FILE *fp);
void perror(const char *s);

int ungetc(int c, FILE *fp);
void clearerr(FILE *fp);
/* rewind: fseek(fp,0,SEEK_SET) plus clearing the error indicator (POSIX). */
void rewind(FILE *fp);
int setvbuf(FILE *fp, char *buf, int mode, size_t size);
FILE *tmpfile(void);

#define getc(fp)    fgetc(fp)
#define putc(c, fp) fputc((c), (fp))

int fprintf(FILE *stream, const char *format, ...) NX_PRINTFLIKE(2, 3);

#endif
