#ifndef _USER_NXASSOC_H
#define _USER_NXASSOC_H

/*
 * user/sys/bin/nxassoc.h
 * File-type -> launcher-program association table (nxfilem rewrite).
 *
 * Header-only, static-inline, same pattern as nxicon.h/nxres.h/nxexec.h:
 * dropped into any translation unit that needs it, no separate .c/link step.
 *
 * This is the single, reusable place that decides WHAT program opens a given
 * file path, by extension. It does NOT decide HOW to launch it — that
 * remains nxexec.h's job (nxexec_spawn_search()/nxexec_spawn_detached()).
 * Before this header, the association was a hardcoded if/else chain private
 * to nxfilem/fileops.c; any other app that wanted "open this file with the
 * right program" (a future nxshell `open <path>`, a desktop double-click
 * elsewhere) had no shared logic to call.
 *
 * Resolution order (nxassoc_resolve):
 *   1. images (os1_image_path_has_known_ext, image.h — reused, not
 *      duplicated) -> /sys/bin/nximage
 *   2. exact-extension match against nxassoc_table[] (case-insensitive,
 *      os1_image_ext_eq from image.h) -> the mapped program, including
 *      ".lua" -> /bin/lua
 *   3. an extensionless path under /bin/ or /sys/bin/ -> itself, executed
 *      directly (NXASSOC_KIND_EXEC, no argv[1])
 *   4. fallback -> /bin/kilo (plain text editor)
 *
 * Directories are the caller's responsibility (nxassoc_resolve never sees
 * one) — nxfilem navigates into a directory instead of "opening" it.
 */

#include <image.h>
#include <os1.h>
#include <string.h>

/* NXASSOC_KIND_ARG: launch "<prog> <path>" (argv[0]=prog, argv[1]=path).
 * NXASSOC_KIND_EXEC: launch "<path>" directly (path IS the program). */
#define NXASSOC_KIND_ARG 0
#define NXASSOC_KIND_EXEC 1

#define NXASSOC_PROG_MAX 32

typedef struct {
  const char *ext;
  const char *prog;
} nxassoc_rule_t;

/* Extension table, longest/most-specific alternatives are not needed here
 * since matching is exact (os1_image_ext_eq), not prefix-based. Ordered by
 * rough popularity, not that order matters for correctness. */
static const nxassoc_rule_t nxassoc_table[] = {
    {".lua", "/bin/lua"},  {".c", "/bin/kilo"},    {".h", "/bin/kilo"},
    {".cpp", "/bin/kilo"}, {".hpp", "/bin/kilo"},  {".s", "/bin/kilo"},
    {".S", "/bin/kilo"},   {".md", "/bin/kilo"},   {".txt", "/bin/kilo"},
    {".log", "/bin/kilo"}, {".cfg", "/bin/kilo"},  {".json", "/bin/kilo"},
    {".ini", "/bin/kilo"}, {".toml", "/bin/kilo"}, {".yaml", "/bin/kilo"},
    {".yml", "/bin/kilo"}, {".html", "/bin/kilo"}, {".htm", "/bin/kilo"},
    {".css", "/bin/kilo"}, {".sh", "/bin/kilo"},   {".py", "/bin/kilo"},
};
#define NXASSOC_NRULES (int)(sizeof(nxassoc_table) / sizeof(nxassoc_table[0]))

/* nxassoc_is_exec_path - an extensionless path directly under /bin/ or
 * /sys/bin/: treated as a launchable executable rather than routed to any
 * editor/viewer. */
static inline int nxassoc_is_exec_path(const char *path) {
  if (!path)
    return 0;
  int under_bin =
      (strncmp(path, "/bin/", 5) == 0 || strncmp(path, "/sys/bin/", 9) == 0);
  if (!under_bin)
    return 0;
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  const char *dot = strrchr(name, '.');
  return dot == NULL || dot == name; /* no extension, or a dotfile */
}

/*
 * nxassoc_resolve - resolve 'path' to a launch program + kind.
 * out_prog (NXASSOC_PROG_MAX bytes) receives the program path (for
 * NXASSOC_KIND_EXEC, out_prog is a copy of 'path' itself).  Returns
 * NXASSOC_KIND_ARG or NXASSOC_KIND_EXEC.  'path' is never a directory
 * (caller's responsibility, see header comment).
 */
static inline int nxassoc_resolve(const char *path, char *out_prog, size_t sz) {
  if (os1_image_path_has_known_ext(path)) {
    snprintf(out_prog, sz, "%s", "/sys/bin/nximage");
    return NXASSOC_KIND_ARG;
  }

  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  const char *ext = strrchr(name, '.');
  if (ext && ext != name) {
    for (int i = 0; i < NXASSOC_NRULES; i++) {
      if (os1_image_ext_eq(ext, nxassoc_table[i].ext)) {
        snprintf(out_prog, sz, "%s", nxassoc_table[i].prog);
        return NXASSOC_KIND_ARG;
      }
    }
  }

  if (nxassoc_is_exec_path(path)) {
    snprintf(out_prog, sz, "%s", path);
    return NXASSOC_KIND_EXEC;
  }

  snprintf(out_prog, sz, "%s", "/bin/kilo");
  return NXASSOC_KIND_ARG;
}

#endif /* _USER_NXASSOC_H */
