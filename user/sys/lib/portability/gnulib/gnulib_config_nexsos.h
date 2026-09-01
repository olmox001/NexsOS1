/*
 * user/sys/lib/portability/gnulib/gnulib_config_nexsos.h
 * NexsOS1 Gnulib target configuration and portability definitions.
 */

#ifndef _GNULIB_CONFIG_NEXSOS_H
#define _GNULIB_CONFIG_NEXSOS_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifndef _NEXSOS_GETCWD_POSIX
#define _NEXSOS_GETCWD_POSIX 1
#endif

/* C99+ supports flexible array members as the last field of a struct */
#ifndef FLEXIBLE_ARRAY_MEMBER
#define FLEXIBLE_ARRAY_MEMBER
#endif

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif

#ifndef UINT_WIDTH
# if defined(__SIZEOF_INT__) && __SIZEOF_INT__ == 4
#  define UINT_WIDTH 32
# elif defined(__SIZEOF_INT__) && __SIZEOF_INT__ == 2
#  define UINT_WIDTH 16
# else
#  define UINT_WIDTH 32
# endif
#endif
#ifndef ULONG_WIDTH
# if defined(__SIZEOF_LONG__) && __SIZEOF_LONG__ == 8
#  define ULONG_WIDTH 64
# elif defined(__SIZEOF_LONG__) && __SIZEOF_LONG__ == 4
#  define ULONG_WIDTH 32
# elif ULONG_MAX == 0xffffffffffffffffUL
#  define ULONG_WIDTH 64
# elif ULONG_MAX == 0xffffffffUL
#  define ULONG_WIDTH 32
# else
#  define ULONG_WIDTH 64
# endif
#endif
#ifndef ULLONG_WIDTH
# if defined(__SIZEOF_LONG_LONG__) && __SIZEOF_LONG_LONG__ == 8
#  define ULLONG_WIDTH 64
# elif defined(__SIZEOF_LONG_LONG__) && __SIZEOF_LONG_LONG__ == 4
#  define ULLONG_WIDTH 32
# else
#  define ULLONG_WIDTH 64
# endif
#endif

/*
 * NexsOS1 getcwd() returns int; POSIX/gnulib expects char *.
 * Use the underlying _sys_getcwd and provide a POSIX shim.
 */
extern int _sys_getcwd(char *buf, size_t size);

static inline char *getcwd(char *buf, size_t size) {
  char *ret = buf;
  if (!buf) {
    size = size ? size : 4096;
    ret = (char *)malloc(size);
    if (!ret) return (char *)0;
  }
  if (_sys_getcwd(ret, size) != 0) {
    if (!buf) free(ret);
    return (char *)0;
  }
  return ret;
}




#ifndef _GL_CONFIG_H_INCLUDED
#define _GL_CONFIG_H_INCLUDED 1
#endif

#ifndef GNULIB_FILENAMECAT
#define GNULIB_FILENAMECAT 1
#endif

#ifndef GNULIB_MBSCHR
#define GNULIB_MBSCHR 1
#endif

#ifndef GNULIB_MBSLEN
#define GNULIB_MBSLEN 1
#endif

#ifndef GNULIB_MBRLEN
#define GNULIB_MBRLEN 1
#endif

#ifndef HAVE_MBSCHR
#define HAVE_MBSCHR 1
#endif

#ifndef HAVE_MBSLEN
#define HAVE_MBSLEN 1
#endif

#ifndef HAVE_MBRLEN
#define HAVE_MBRLEN 1
#endif

#ifndef PROMOTED_MODE_T
#define PROMOTED_MODE_T mode_t
#endif



#ifndef GNULIB_DIRNAME
#define GNULIB_DIRNAME 1
#endif

#ifndef GNULIB_BASENAME
#define GNULIB_BASENAME 1
#endif

#ifndef GNULIB_XALLOC
#define GNULIB_XALLOC 1
#endif

#ifndef GNULIB_XALLOC_DIE
#define GNULIB_XALLOC_DIE 1
#endif

#ifndef GNULIB_FOPEN_SAFER
#define GNULIB_FOPEN_SAFER 1
#endif

#ifndef GNULIB_FREOPEN_SAFER
#define GNULIB_FREOPEN_SAFER 1
#endif

#ifndef GNULIB_POPEN_SAFER
#define GNULIB_POPEN_SAFER 1
#endif

#ifndef GNULIB_TMPFILE_SAFER
#define GNULIB_TMPFILE_SAFER 1
#endif

#ifndef __getopt_argv_const
#define __getopt_argv_const const
#endif

#ifndef _GL_CMP
#define _GL_CMP(a, b) (((a) > (b)) - ((a) < (b)))
#endif


#ifndef alignof
#define alignof _Alignof
#endif

#ifndef static_assert
#define static_assert(expr, ...) _Static_assert(expr, "" #expr)
#endif


#ifndef UINT_WIDTH
#define UINT_WIDTH 32
#endif


#ifndef MB_CUR_MAX
#define MB_CUR_MAX 1
#endif

#ifndef mbsinit
#define mbsinit(ps) 1
#endif

#include <wchar.h>
#include <string.h>

#ifndef mbrtoc32
#define mbrtoc32(pwc, s, n, ps) mbrtowc((wchar_t *)(pwc), s, n, ps)
#endif

/* memeq — gnulib uses this throughout; provided both as inline and extern */
#ifndef GNULIB_defined_memeq
#ifndef _GNULIB_OS1_GLUE_IMPL
/* Provide as static inline for normal compilation units */
#include <string.h>
static inline int
memeq(void const *s1, void const *s2, size_t n)
{
  return memcmp(s1, s2, n) == 0;
}
#else
/* In glue.c, declare as extern (defined in same file) */
extern int memeq(const void *s1, const void *s2, size_t n);
#endif
#define GNULIB_defined_memeq 1
#endif

/* streq — gnulib/coreutils string equality shorthand */
#ifndef GNULIB_defined_streq
static inline int streq(const char *s1, const char *s2) {
  return strcmp(s1, s2) == 0;
}
#define GNULIB_defined_streq 1
#endif

/* fpurge — not available on NexsOS1 */
#ifndef fpurge
#define fpurge(fp) ((void)0)
#endif

/* c32isspace — NexsOS1 has no C32 char classification */
#ifndef c32isspace
#define c32isspace(c) isspace((int)(c))
#endif



/* mbszero — zero-initialize mbstate */
#ifndef mbszero
#define mbszero(ps) memset((ps), 0, sizeof(mbstate_t))
#endif


#ifndef c32isprint
#define c32isprint(c) isprint(c)
#endif

#ifndef _IDX_T_DEFINED
#define _IDX_T_DEFINED 1
typedef ptrdiff_t idx_t;
#endif

#ifndef _GL_GNUC_PREREQ
#if defined __GNUC__ && defined __GNUC_MINOR__
# define _GL_GNUC_PREREQ(maj, min) \
    ((maj) < __GNUC__ + ((min) <= __GNUC_MINOR__))
#else
# define _GL_GNUC_PREREQ(maj, min) 0
#endif
#endif


#ifndef HAVE_NANOSLEEP
#define HAVE_NANOSLEEP 1
#endif

#ifndef HAVE_WORKING_O_NOFOLLOW
#define HAVE_WORKING_O_NOFOLLOW 0
#endif

#ifndef GNULIB_TEXT_DOMAIN
#define GNULIB_TEXT_DOMAIN "gnulib"
#endif




#ifndef PACKAGE
#define PACKAGE "coreutils"
#endif

#ifndef PACKAGE_NAME
#define PACKAGE_NAME "GNU coreutils"
#endif

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "9.5"
#endif

#ifndef PACKAGE_BUGREPORT
#define PACKAGE_BUGREPORT "bug-coreutils@gnu.org"
#endif

#ifndef PACKAGE_URL
#define PACKAGE_URL "https://www.gnu.org/software/coreutils/"
#endif

#ifndef PACKAGE_PACKAGER

#define PACKAGE_PACKAGER "NexsOS1"
#endif

#ifndef PACKAGE_PACKAGER_VERSION
#define PACKAGE_PACKAGER_VERSION "0.0.5.4"
#endif

#ifndef PACKAGE_PACKAGER_BUG_REPORTS
#define PACKAGE_PACKAGER_BUG_REPORTS "https://github.com/olmox001/NexsOS1"
#endif

#ifndef ENABLE_NLS
#define ENABLE_NLS 0
#endif

#ifndef __NEXSOS__
#define __NEXSOS__ 1
#endif





#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/* Standard type and header guarantees on NexsOS1 (LP64) */
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDIO_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_TIME_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1

/* Non-POSIX stat type tests not provided by NexsOS1 */
#ifndef S_ISCTG
#define S_ISCTG(m) 0
#endif
#ifndef S_ISDOOR
#define S_ISDOOR(m) 0
#endif
#ifndef S_ISMPB
#define S_ISMPB(m) 0
#endif
#ifndef S_ISMPC
#define S_ISMPC(m) 0
#endif
#ifndef S_ISMPX
#define S_ISMPX(m) 0
#endif
#ifndef S_ISNWK
#define S_ISNWK(m) 0
#endif
#ifndef S_ISPORT
#define S_ISPORT(m) 0
#endif
#ifndef S_ISWHT
#define S_ISWHT(m) 0
#endif
#ifndef S_TYPEISSEM
#define S_TYPEISSEM(p) 0
#endif
#ifndef S_TYPEISMQ
#define S_TYPEISMQ(p) 0
#endif
#ifndef S_TYPEISSHM
#define S_TYPEISSHM(p) 0
#endif
#ifndef S_TYPEISTMO
#define S_TYPEISTMO(p) 0
#endif

#define HAVE_SYS_WAIT_H 1
#define HAVE_DIRENT_H 1
#define HAVE_ERRNO_H 1
#define HAVE_ASSERT_H 1
#define HAVE_CTYPE_H 1
#define HAVE_MATH_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1

/* Standard C runtime functions available in NexsOS1 lib.c */
#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_MEMCHR 1
#define HAVE_MEMCMP 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_STRCPY 1
#define HAVE_STRNCPY 1
#define HAVE_STRCAT 1
#define HAVE_STRNCAT 1
#define HAVE_STRLEN 1
#define HAVE_STRDUP 1
#define HAVE_STRSTR 1
#define HAVE_STRTOL 1
#define HAVE_SNPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_SSCANF 1
#define HAVE_FOPEN 1
#define HAVE_FCLOSE 1
#define HAVE_FREAD 1
#define HAVE_FWRITE 1
#define HAVE_FSEEK 1
#define HAVE_FTELL 1
#define HAVE_FFLUSH 1
#define HAVE_GETPID 1
#define HAVE_NANOSLEEP 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_STAT 1
#define HAVE_MKDIR 1
#define HAVE_RENAME 1
#define HAVE_UNLINK 1
#define HAVE_ISATTY 1
#define HAVE_GETCWD 1
#define HAVE_CHDIR 1
#define HAVE_PIPE 1
#define HAVE_DUP 1
#define HAVE_DUP2 1
#define HAVE_RAW_DECL_MEMRCHR 1
#define HAVE_RAW_DECL_RAWMEMCHR 1

/* Timestamp support for touch / coreutils */
#define HAVE_UTIME              1
#define HAVE_UTIMES             1
#define HAVE_UTIMENSAT          1
#define HAVE_FUTIMENS           1
#define HAVE_STRUCT_TIMESPEC    1
#define HAVE_STRUCT_UTIMBUF     1
#define HAVE_STRUCT_TIMEVAL     1

/* Architecture bitness */
#define SIZEOF_VOID_P 8
#define SIZEOF_SIZE_T 8
#define SIZEOF_LONG 8
#define SIZEOF_INT 4
#define SIZEOF_SHORT 2
#define SIZEOF_OFF_T 8

/* Compiler macro helpers */
#ifndef _GL_INLINE_HEADER_BEGIN
#define _GL_INLINE_HEADER_BEGIN
#endif

#ifndef _GL_INLINE_HEADER_END
#define _GL_INLINE_HEADER_END
#endif

#ifndef _GL_ATTRIBUTE_PURE
#if __GNUC__ >= 3
#define _GL_ATTRIBUTE_PURE __attribute__((__pure__))
#else
#define _GL_ATTRIBUTE_PURE
#endif
#endif

#ifndef _GL_ATTRIBUTE_CONST
#if __GNUC__ >= 3
#define _GL_ATTRIBUTE_CONST __attribute__((__const__))
#else
#define _GL_ATTRIBUTE_CONST
#endif
#endif

#ifndef _GL_ATTRIBUTE_MALLOC
#if __GNUC__ >= 3
#define _GL_ATTRIBUTE_MALLOC __attribute__((__malloc__))
#else
#define _GL_ATTRIBUTE_MALLOC
#endif
#endif

#ifndef _GL_ATTRIBUTE_DEALLOC
#define _GL_ATTRIBUTE_DEALLOC(f, i)
#endif

#ifndef _GL_ATTRIBUTE_DEALLOC_FREE
#define _GL_ATTRIBUTE_DEALLOC_FREE
#endif

#ifndef _GL_ATTRIBUTE_RETURNS_NONNULL
#if __GNUC__ >= 5
#define _GL_ATTRIBUTE_RETURNS_NONNULL __attribute__((__returns_nonnull__))
#else
#define _GL_ATTRIBUTE_RETURNS_NONNULL
#endif
#endif

#ifndef _GL_UNUSED
#if __GNUC__ >= 3
#define _GL_UNUSED __attribute__((__unused__))
#else
#define _GL_UNUSED
#endif
#endif

#ifndef _GL_ATTRIBUTE_NODISCARD
#if __GNUC__ >= 4
#define _GL_ATTRIBUTE_NODISCARD __attribute__((__warn_unused_result__))
#else
#define _GL_ATTRIBUTE_NODISCARD
#endif
#endif

#ifndef _GL_ATTRIBUTE_MAYBE_UNUSED
#define _GL_ATTRIBUTE_MAYBE_UNUSED _GL_UNUSED
#endif

#ifndef _GL_ATTRIBUTE_NONNULL
#if __GNUC__ >= 3
#define _GL_ATTRIBUTE_NONNULL(args) __attribute__((__nonnull__ args))
#else
#define _GL_ATTRIBUTE_NONNULL(args)
#endif
#endif

#ifndef _GL_ARG_NONNULL
#define _GL_ARG_NONNULL(params) _GL_ATTRIBUTE_NONNULL(params)
#endif

#ifndef _GL_ATTRIBUTE_FORMAT
#if __GNUC__ >= 3
#define _GL_ATTRIBUTE_FORMAT(spec) __attribute__((__format__ spec))
#else
#define _GL_ATTRIBUTE_FORMAT(spec)
#endif
#endif

#ifndef _GL_EXTERN_INLINE
#define _GL_EXTERN_INLINE static inline
#endif

#ifndef _GL_INLINE
#define _GL_INLINE static inline
#endif

#ifndef _GL_ATTRIBUTE_COUNTED_BY
#define _GL_ATTRIBUTE_COUNTED_BY(x)
#endif

#ifndef _GL_UNNAMED
#define _GL_UNNAMED(x) x
#endif

#ifndef _GL_ATTRIBUTE_ALLOC_SIZE
#define _GL_ATTRIBUTE_ALLOC_SIZE(x)
#endif

#ifndef _GL_ATTRIBUTE_ALWAYS_INLINE
#define _GL_ATTRIBUTE_ALWAYS_INLINE static inline
#endif

#ifndef _GL_ATTRIBUTE_ARTIFICIAL
#define _GL_ATTRIBUTE_ARTIFICIAL
#endif

#ifndef _GL_ATTRIBUTE_COLD
#define _GL_ATTRIBUTE_COLD
#endif

#ifndef _GL_ATTRIBUTE_DEPRECATED
#define _GL_ATTRIBUTE_DEPRECATED
#endif

#ifndef _GL_ATTRIBUTE_ERROR
#define _GL_ATTRIBUTE_ERROR(x)
#endif

#ifndef _GL_ATTRIBUTE_WARNING
#define _GL_ATTRIBUTE_WARNING(x)
#endif

#ifndef _GL_ATTRIBUTE_EXTERNALLY_VISIBLE
#define _GL_ATTRIBUTE_EXTERNALLY_VISIBLE
#endif

#ifndef _GL_ATTRIBUTE_FALLTHROUGH
#define _GL_ATTRIBUTE_FALLTHROUGH
#endif

#ifndef _GL_ATTRIBUTE_LEAF
#define _GL_ATTRIBUTE_LEAF
#endif

#ifndef _GL_ATTRIBUTE_MAY_ALIAS
#define _GL_ATTRIBUTE_MAY_ALIAS
#endif

#ifndef _GL_ATTRIBUTE_NOINLINE
#define _GL_ATTRIBUTE_NOINLINE
#endif

#ifndef _GL_ATTRIBUTE_NONNULL_IF_NONZERO
#define _GL_ATTRIBUTE_NONNULL_IF_NONZERO(x)
#endif

#ifndef _GL_ATTRIBUTE_NONSTRING
#define _GL_ATTRIBUTE_NONSTRING
#endif

#ifndef _GL_ATTRIBUTE_NOTHROW
#define _GL_ATTRIBUTE_NOTHROW
#endif

#ifndef _GL_ATTRIBUTE_PACKED
#define _GL_ATTRIBUTE_PACKED __attribute__((__packed__))
#endif

#ifndef _GL_ATTRIBUTE_REPRODUCIBLE
#define _GL_ATTRIBUTE_REPRODUCIBLE
#endif

#ifndef _GL_ATTRIBUTE_SENTINEL
#define _GL_ATTRIBUTE_SENTINEL(x)
#endif

#ifndef _GL_ATTRIBUTE_UNSEQUENCED
#define _GL_ATTRIBUTE_UNSEQUENCED
#endif

#ifndef _Noreturn
#define _Noreturn __attribute__((__noreturn__))
#endif


/* Include standard OS1 definitions */
#include <os1.h>
#include <errno.h>

#endif /* _GNULIB_CONFIG_NEXSOS_H */
