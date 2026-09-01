/* Compatibility generation for GNU libc scratch buffer internals.
   NexsOS1 uses the upstream Gnulib sources directly and expects the generated
   header that glibc-internal/scratch_buffer would normally create.  This file
   provides the minimal API surface required by canonicalize.c and similar
   helpers. */

#ifndef _GL_SCRATCH_BUFFER_GL_H
#define _GL_SCRATCH_BUFFER_GL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#ifndef _GL_LIKELY
# define _GL_LIKELY(cond) __builtin_expect ((cond), 1)
# define _GL_UNLIKELY(cond) __builtin_expect ((cond), 0)
#endif

#ifndef __GLIBC_LIKELY
# define __GLIBC_LIKELY(cond) _GL_LIKELY (cond)
# define __GLIBC_UNLIKELY(cond) _GL_UNLIKELY (cond)
#endif

#define __libc_scratch_buffer_grow _gl_scratch_buffer_grow
#define __libc_scratch_buffer_grow_preserve _gl_scratch_buffer_grow_preserve
#define __libc_scratch_buffer_set_array_size _gl_scratch_buffer_set_array_size

struct scratch_buffer {
  void *data;
  size_t length;
  union { long double __align; char __c[1024]; } __space;
};

static inline void
scratch_buffer_init (struct scratch_buffer *buffer)
{
  buffer->data = buffer->__space.__c;
  buffer->length = sizeof (buffer->__space);
}

static inline void
scratch_buffer_free (struct scratch_buffer *buffer)
{
  if (buffer->data != buffer->__space.__c)
    free (buffer->data);
}

bool __libc_scratch_buffer_grow (struct scratch_buffer *buffer);
static __inline__ bool
scratch_buffer_grow (struct scratch_buffer *buffer)
{
  return __GLIBC_LIKELY (__libc_scratch_buffer_grow (buffer));
}

bool __libc_scratch_buffer_grow_preserve (struct scratch_buffer *buffer);
static __inline__ bool
scratch_buffer_grow_preserve (struct scratch_buffer *buffer)
{
  return __GLIBC_LIKELY (__libc_scratch_buffer_grow_preserve (buffer));
}

bool __libc_scratch_buffer_set_array_size (struct scratch_buffer *buffer,
                                           size_t nelem, size_t size);
static __inline__ bool
scratch_buffer_set_array_size (struct scratch_buffer *buffer,
                              size_t nelem, size_t size)
{
  return __GLIBC_LIKELY (__libc_scratch_buffer_set_array_size (buffer, nelem, size));
}

#endif
