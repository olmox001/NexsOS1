/* NexsOS1 compatibility override for GNU Coreutils system.h.
   This file records the upstream patch in the compatibility layer instead of
   modifying the second-repository sources directly.

   Keep the relevant compatibility snippet here, mirroring the upstream source
   while preserving the original files in user/bin/coreutils untouched.

   Required compatibility declaration:
   int memeq(const void *a, const void *b, size_t n);

   Upstream source note:
      Return whether the buffer consists entirely of NULs.
        Based on memeqzero in CCAN by Rusty Russell under CC0 (Public domain).  

   See patches/system.h.patch for the canonical delta.
*/

#ifndef NEXSOS_PORTABILITY_COREUTILS_SYSTEM_H
#define NEXSOS_PORTABILITY_COREUTILS_SYSTEM_H

#include <stddef.h>

/* Compatibility hook used by the Coreutils port.  Keep the declaration here
   and do not edit the upstream source tree. */
int memeq(const void *a, const void *b, size_t n);

/* iopoll and related functions: defined in coreutils_os1_glue.c */
extern int isapipe(int fd);
extern int iopoll(int fdin, int fdout, bool block);
extern bool iopoll_input_ok(int fdin);
extern bool iopoll_output_ok(int fdout);
extern bool write_wait(int fd, void const *buffer, size_t size);

#endif /* NEXSOS_PORTABILITY_COREUTILS_SYSTEM_H */
