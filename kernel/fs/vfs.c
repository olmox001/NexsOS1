/*
 * kernel/fs/vfs.c
 * Virtual Filesystem Switch — Path Normalisation Shim
 *
 * Purpose:
 *   Provides vfs_resolve_path(), the sole VFS-layer entry point.  It converts
 *   a raw user-supplied path (absolute or relative) into a canonical absolute
 *   path string suitable for passing to ext4_find_inode().
 *
 * Role in the stack:
 *   Called exclusively from syscall_dispatch.c (cases 251–256) before each
 *   ext4_* call.  The caller then invokes ext4_find_inode / ext4_read_inode /
 *   ext4_write_file directly — there is no vnode table, no mount table, and
 *   no filesystem-type abstraction between this shim and the ext4 driver.
 *
 * Key invariants:
 *   - Output buffer is always NUL-terminated (explicit out[size-1]='\0' at L58).
 *   - Normalisation is done entirely in-place on a stack copy; the original
 *     'in' string is never modified.
 *   - parts[] stores interior pointers into temp[]; they remain valid for the
 *     lifetime of the normalisation loop because temp[] is on the same frame.
 *
 * Known issues:
 *   VFS-01  (W5 WRONG-DESIGN) The VFS layer is a single path-normalisation
 *           function.  There is no vnode, no mount table, and no file_ops
 *           dispatch.  All FS syscalls reach ext4_* directly via extern
 *           declarations.  This is the primary structural blocker to the
 *           stated Plan 9 / seL4-capability goal.
 *   VFS-02  (W3 SECURITY+MISSING) current_process is dereferenced without a
 *           NULL guard.  Safe only because all callers are on validated syscall
 *           paths; the function itself has no protection.
 *   VFS-03  (W2 BAD-IMPL) parts[32] limits the resolved path to 32 components;
 *           deeper paths are silently truncated with no error returned.
 *   VFS-04  (W1 REFINE) temp[256] and normalized[256] are both stack-allocated.
 *           A CWD of 127 bytes + in of 128 bytes = 255 chars before the NUL;
 *           strncat guards the overflow but silently truncates the result.
 */
#include <kernel/types.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/printk.h>

/*
 * vfs_resolve_path - canonicalise a raw user path into an absolute path string.
 *
 * @in:   input path (absolute or relative; not modified).
 * @out:  output buffer; receives the NUL-terminated absolute path.
 * @size: capacity of out[], including the NUL terminator.
 *
 * Preconditions:
 *   - out != NULL, size >= 1.
 *   - If in is relative, current_process must be non-NULL and
 *     current_process->cwd must be a valid NUL-terminated string.
 *     NOTE(VFS-02): neither current_process nor cwd is null-checked here.
 *
 * Algorithm:
 *   1. Build temp[] = (absolute ? in : cwd + "/" + in), truncated to 255 chars.
 *   2. Walk temp[] splitting on '/', accumulating component pointers in
 *      parts[0..31].  ".." decrements part_count; "." is skipped.
 *      NOTE(VFS-03): parts[] has only 32 slots; the 33rd and later components
 *      are silently dropped — no error is returned to the caller.
 *   3. Reassemble parts[] into normalized[] with leading '/'.
 *   4. strncpy into out[]; force-NUL-terminate at out[size-1].
 *      NOTE(VFS-04): Both temp[] and normalized[] are 256-byte stack buffers.
 *      A CWD near 128 bytes joined with 'in' near 128 bytes will be silently
 *      truncated by strncat before the normalisation loop starts.
 *
 * Side effects: none (no disk I/O, no locks).
 *
 * Returns: void; result is written to out[].
 *
 * Path Resolution:
 * - If path starts with '/', it's absolute.
 * - Otherwise, it's relative to current_process->cwd.
 * - Normalizes . and ..
 */
void vfs_resolve_path(const char *in, char *out, size_t size) {
    /* temp[]: working copy of the unsplit path; max 255 chars + NUL.
     * NOTE(VFS-04): 256-byte stack buffer; CWD + "/" + in > 255 chars
     * will be silently truncated by strncat (the -1 guard below). */
    char temp[256];
    if (in[0] == '/') {
        /* Absolute path: copy verbatim into temp; strncpy NUL-pads. */
        strncpy(temp, in, sizeof(temp));
    } else {
        /* Relative path: prepend current working directory.
         * NOTE(VFS-02): current_process not null-checked before deref. */
        strncpy(temp, current_process->cwd, sizeof(temp));
        size_t len = strlen(temp);
        /* Ensure exactly one '/' separator between CWD and 'in'. */
        if (len > 0 && temp[len-1] != '/') {
            strncat(temp, "/", sizeof(temp) - len - 1);
        }
        strncat(temp, in, sizeof(temp) - strlen(temp) - 1);
    }
    /* Force NUL-termination in case strncpy filled all 256 bytes. */
    temp[sizeof(temp)-1] = '\0';

    /* Normalize path: remove redundant /./ and handle /../ */
    /* normalized[]: final assembled canonical path; same 256-byte limit.
     * NOTE(VFS-04): no growth beyond 255 chars; deep paths are truncated
     * silently by strncat's capacity guard in the assembly loop below. */
    char normalized[256];
    /* parts[32]: array of interior pointers into temp[], each pointing to
     * one decoded path component (NUL-terminated in-place at the '/' char).
     * NOTE(VFS-03): capped at 32 slots; the 33rd component is silently
     * dropped because the (part_count < 32) guard skips the assignment. */
    char *parts[32];
    int part_count = 0;

    char *s = temp;
    /* Skip the leading '/' so strtok-style splitting starts on the first
     * component name, not an empty string before the root slash. */
    if (*s == '/') s++;

    char *token = s;
    while (token && *token) {
        /* NUL-terminate this component in-place; next points past the '/'. */
        char *next = strchr(token, '/');
        if (next) *next = '\0';

        if (strcmp(token, "..") == 0) {
            /* Parent: pop last component (floor at root, never below 0). */
            if (part_count > 0) part_count--;
        } else if (strcmp(token, ".") != 0 && strlen(token) > 0) {
            /* Normal component: store interior pointer into temp[].
             * The pointer is valid until the function returns (temp is on
             * the same stack frame and is not modified after this loop). */
            if (part_count < 32) parts[part_count++] = token;
        }

        if (next) token = next + 1;
        else break;
    }

    /* Reassemble: start with root '/' then append each component. */
    strcpy(normalized, "/");
    for (int i = 0; i < part_count; i++) {
        strncat(normalized, parts[i], sizeof(normalized) - strlen(normalized) - 1);
        /* Insert '/' between components but not after the last one. */
        if (i < part_count - 1) strncat(normalized, "/", sizeof(normalized) - strlen(normalized) - 1);
    }

    strncpy(out, normalized, size);
    /* Guarantee NUL-termination even if strncpy ran out of space. */
    out[size-1] = '\0';
}
