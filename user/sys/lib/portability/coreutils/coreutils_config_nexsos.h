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
#undef HAVE_SELINUX_SELINUX_H
#undef HAVE_SYS_ACL_H
#undef HAVE_SYS_XATTR_H
#define HAVE_SYS_STATFS_H 1
#define HAVE_SYS_STATVFS_H 1
#undef HAVE_SYS_VFS_H
#undef HAVE_SYS_MOUNT_H
#undef HAVE_SYS_PARAM_H
#define STAT_STATVFS 1
#define HAVE_STRUCT_STATVFS_F_TYPE 1
#define HAVE_STRUCT_STATVFS_F_FSID 1
#define HAVE_STRUCT_STATVFS_F_NAMEMAX 1
#define HAVE_STRUCT_STATVFS_F_FRSIZE 1
#undef HAVE_STRUCT_STATFS_F_TYPE
#undef HAVE_STRUCT_STATFS_F_FSTYPENAME
#undef HAVE_STRUCT_STATFS_F_NAMELEN
#undef HAVE_STRUCT_STATFS_F_NAMEMAX
#undef HAVE_STRUCT_STATFS_F_FRSIZE

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

#ifndef lint
#define lint 1
#endif

#ifndef mbszero
#define mbszero(ps) memset(ps, 0, sizeof(*(ps)))
#define GNULIB_defined_mbszero 1
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
