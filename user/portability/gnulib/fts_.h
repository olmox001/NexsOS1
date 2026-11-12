/*
 * user/sys/lib/portability/gnulib/fts_.h
 * Shim for Gnulib's fts_.h, generated from fts.in.h
 * NexsOS1 doesn't have a real fts implementation, so this provides minimal stubs.
 */

#ifndef FTS_H
#define FTS_H

#include <sys/types.h>
#include <dirent.h>

/* FTS entry types */
#define FTS_D     1    /* preorder directory */
#define FTS_DC    2    /* directory that causes cycles */
#define FTS_DEFAULT 3  /* none of the above */
#define FTS_DNR   4    /* unreadable directory */
#define FTS_DOT   5    /* dot or dot-dot */
#define FTS_DP    6    /* postorder directory */
#define FTS_ERR   7    /* error; errno is set */
#define FTS_F     8    /* regular file */
#define FTS_INIT  9    /* initialized only */
#define FTS_NS   10    /* stat(2) failed */
#define FTS_NSOK 11    /* no stat(2) requested */
#define FTS_SL   12    /* symbolic link */
#define FTS_SLNONE 13  /* symbolic link with no file */
#define FTS_W    14    /* whiteout object */

/* FTS options */
#define FTS_COMFOLLOW  0x0001
#define FTS_LOGICAL    0x0002
#define FTS_NOCHDIR    0x0004
#define FTS_NOSTAT     0x0008
#define FTS_PHYSICAL   0x0010
#define FTS_SEEDOT     0x0020
#define FTS_XDEV       0x0040
#define FTS_WHITEOUT   0x0080
#define FTS_MOUNT      0x0100
#define FTS_CWDFD      0x0200
#define FTS_DEFER_STAT 0x0400
#define FTS_OPTIONMASK 0x0fff

#define FTS_NAMEONLY   0x100
#define FTS_STOP       0x200

/* FTS levels */
#define FTS_ROOTPARENTLEVEL -1
#define FTS_ROOTLEVEL        0

typedef struct {
    struct _ftsent *fts_cur;
    struct _ftsent *fts_child;
    struct _ftsent **fts_array;
    dev_t fts_dev;
    char *fts_path;
    int fts_rfd;
    int fts_cwd_fd;  /* current working directory file descriptor */
    size_t fts_pathlen;
    size_t fts_nitems;
    int (*fts_compar)();
    int fts_options;
} FTS;

typedef struct _ftsent {
    struct _ftsent *fts_cycle;
    struct _ftsent *fts_parent;
    struct _ftsent *fts_link;
    long fts_number;
    void *fts_pointer;
    char *fts_accpath;
    char *fts_path;
    int fts_errno;
    int fts_symfd;
    unsigned short fts_pathlen;
    unsigned short fts_namelen;
    unsigned short fts_level;
    unsigned int fts_info;
    unsigned int fts_flags;
    unsigned int fts_instr;
    struct stat *fts_statp;
    char fts_name[1];
} FTSENT;

/* FTS entry instructions */
#define FTS_AGAIN   1
#define FTS_FOLLOW  2
#define FTS_NOINSTR 3
#define FTS_SKIP    4

/* Function declarations (stubs for NexsOS1) */
FTS *fts_open(char * const *argv, int options, int (*compar)(const FTSENT **, const FTSENT **));
FTSENT *fts_read(FTS *sp);
FTSENT *fts_children(FTS *sp, int instr);
int fts_set(FTS *sp, FTSENT *p, int instr);
int fts_close(FTS *sp);

#endif /* FTS_H */
