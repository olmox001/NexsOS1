/*
 * user/sys/lib/portability/coreutils/coreutils_config_nexsos.h
 * NexsOS1 Coreutils portability and target configuration.
 */

#ifndef _COREUTILS_CONFIG_NEXSOS_H
#define _COREUTILS_CONFIG_NEXSOS_H

#include "gnulib_config_nexsos.h"
#include "coreutils_os1_glue.h"

#define PACKAGE "coreutils"

#define PACKAGE_NAME "GNU coreutils"
#define PACKAGE_VERSION "9.5"
#define VERSION "9.5"
#define PACKAGE_STRING "GNU coreutils 9.5"
#define PACKAGE_BUGREPORT "bug-coreutils@gnu.org"
#define PACKAGE_URL "https://www.gnu.org/software/coreutils/"
#define HOST_OPERATING_SYSTEM "NexsOS1"


/* Disable host security subsystems not applicable to bare-metal / NexsOS1 */
#define HAVE_SELINUX_SELINUX_H 0
#define HAVE_SYS_ACL_H 0
#define HAVE_SYS_XATTR_H 0

#ifndef unreachable
# if defined(__GNUC__) || defined(__clang__)
#  define unreachable() __builtin_unreachable()
# else
#  define unreachable() abort()
# endif
#endif

#ifndef c32isspace
#define c32isspace(c) isspace((int)(c))
#endif
#ifndef mbszero

#define mbszero(ps) memset(ps, 0, sizeof(*(ps)))
#endif
#ifndef streq
#define streq(a, b) (strcmp(a, b) == 0)
#endif
#ifndef S_TYPEISSHM
#define S_TYPEISSHM(p) 0
#endif
#ifndef S_TYPEISTMO
#define S_TYPEISTMO(p) 0
#endif
#ifndef fpurge
#define fpurge(fp) fflush(fp)
#endif


#endif /* _COREUTILS_CONFIG_NEXSOS_H */
