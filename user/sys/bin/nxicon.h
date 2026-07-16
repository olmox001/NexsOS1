#ifndef _USER_NXICON_H
#define _USER_NXICON_H

/*
 * user/sys/bin/nxicon.h
 * Dock/launcher icon loading and caching (GFX-NXUI-02 follow-up).
 *
 * Icons live on the VFS at /home/Pictures/icon/{dark,light}/<name>.png — a
 * curated, OS-shipped asset set (NOT arbitrary user images), two full sets so a
 * theme flip never needs to touch disk again once both are warm.  This header
 * owns the one place that knows those paths, which app a window title/exe path
 * maps to, and how the tile-sized ARGB copy is cached — nxui.c (and later
 * nxlauncher.c) just calls nxicon_get() and draws whatever it returns.
 *
 * Same header-only static-inline pattern as nxres.h/nxexec.h: dropped into
 * any translation unit that needs it, no separate .c/link step.
 *
 * TOGGLE (NXICON_ENABLE): compile-time only, on purpose.  With it at 0,
 * nxicon_get() always returns NULL and every caller's existing classic-tile
 * fallback (flat rounded-square fill, unchanged since GFX-NXUI-02) runs
 * instead — one guarded lookup, not a second maintained code path.  A
 * missing/undecodable individual icon degrades the SAME way at runtime
 * (nxicon_get returns NULL for just that one), so "some icons installed"
 * and "icons off entirely" are handled by the exact same caller-side check.
 *
 * SAFETY NOTE: nxicon_get() still calls os1_image_load(), which still runs
 * the full sanitizing decode in graphics_load_image() (bounded read off a
 * capability-gated handle, stbi_info probe, dimension/pixel caps) — that
 * path is not weakened for icons.  What IS skipped, because these are known
 * square OS-shipped assets rather than arbitrary user photos, is nximage.c's
 * aspect-preserving os1_image_fit_size()/os1_image_resample_fit() dance:
 * icons resample straight to tile_size x tile_size, once, then are cached —
 * never re-decoded or re-fit per frame, which matters here because nxui
 * redraws potentially many tiles at ~30Hz.
 */

#include <image.h>
#include <os1.h>
#include <string.h>

#ifndef NXICON_ENABLE
#define NXICON_ENABLE 1
#endif

/* App identity an icon is drawn for.  NXICON_UNKNOWN is a real, drawable icon
 * (the generic "unknown app" glyph, backed by fallback.png) — not to be
 * confused with nxicon_get() returning NULL, which means "no icon available
 * at all, use the classic flat tile".
 *
 * One id per compiled program that has a matching asset in the icon set.
 * Two naming mismatches worth flagging explicitly since they're easy to
 * "fix" by mistake later:
 *   - NXICON_MEMSTAT's binary is nxmemstat, but its icon is nxmeminfo.png.
 *   - NXICON_TEXTEDIT's binary is kilo (the editor from user/bin), but its
 *     icon is textedit.png — there is no "kilo.png".
 * Programs with no matching asset (nxbar, nxinit, nxproc, nxres, nxwins,
 * nxexec, nxnotify, nxntfy_srv, demo3d, and the test/stress binaries in
 * user/bin) simply aren't listed here and fall through to NXICON_UNKNOWN
 * via nxicon_classify*(), same as always. */
#define NXICON_LAUNCHER 0
#define NXICON_SHELL 1
#define NXICON_FILEM 2
#define NXICON_IMAGE 3
#define NXICON_INFO 4
#define NXICON_MEMSTAT 5
#define NXICON_PERM 6
#define NXICON_POWER 7
#define NXICON_REG 8
#define NXICON_SETTINGS 9
#define NXICON_TOP 10
#define NXICON_DOOM 11
#define NXICON_RAPTOR 12
#define NXICON_TEXTEDIT 13
/* Per-FILETYPE icons (by extension/kind, NOT by app id) — landed for
 * nxfilem's content list, see nxicon_classify_file() below.  folder.png is
 * used for directories; the file_* set covers the extension buckets
 * nxfilem's association table (nxassoc.h) already distinguishes. */
#define NXICON_FOLDER 14
#define NXICON_FILE_C 15
#define NXICON_FILE_TEXT 16
#define NXICON_FILE_CONFIG 17
#define NXICON_FILE_HTML 18
#define NXICON_FILE_SCRIPT 19
#define NXICON_FILE_IMAGE 20
#define NXICON_FILE_VIDEO 21
#define NXICON_FILE_BINARY 22
#define NXICON_FILE_EXECUTABLE 23
#define NXICON_FILE_GENERIC 24
#define NXICON_UNKNOWN 25
#define NXICON_COUNT 26
/* Still not wired: nxscript.png (no program named nxscript yet — likely
 * meant for a future scripting app, distinct from file_script.png above). */

#define NXICON_THEME_DARK 0
#define NXICON_THEME_LIGHT 1

static const char *const nxicon_paths[2][NXICON_COUNT] = {
    /* dark */
    {
        "/home/Pictures/icon/dark/nxlauncher.png",
        "/home/Pictures/icon/dark/nxshell.png",
        "/home/Pictures/icon/dark/nxfile.png",
        "/home/Pictures/icon/dark/nximage.png",
        "/home/Pictures/icon/dark/nxinfo.png",
        "/home/Pictures/icon/dark/nxmeminfo.png",
        "/home/Pictures/icon/dark/nxperm.png",
        "/home/Pictures/icon/dark/nxpower.png",
        "/home/Pictures/icon/dark/nxreg.png",
        "/home/Pictures/icon/dark/nxsettings.png",
        "/home/Pictures/icon/dark/nxtop.png",
        "/home/Pictures/icon/dark/doom.png",
        "/home/Pictures/icon/dark/raptor.png",
        "/home/Pictures/icon/dark/textedit.png",
        "/home/Pictures/icon/dark/folder.png",
        "/home/Pictures/icon/dark/file_c.png",
        "/home/Pictures/icon/dark/file_text.png",
        "/home/Pictures/icon/dark/file_config.png",
        "/home/Pictures/icon/dark/file_html.png",
        "/home/Pictures/icon/dark/file_script.png",
        "/home/Pictures/icon/dark/file_image.png",
        "/home/Pictures/icon/dark/file_video.png",
        "/home/Pictures/icon/dark/file_binary.png",
        "/home/Pictures/icon/dark/file_executable.png",
        "/home/Pictures/icon/dark/file_generic.png",
        "/home/Pictures/icon/dark/fallback.png",
    },
    /* light */
    {
        "/home/Pictures/icon/light/nxlauncher.png",
        "/home/Pictures/icon/light/nxshell.png",
        "/home/Pictures/icon/light/nxfile.png",
        "/home/Pictures/icon/light/nximage.png",
        "/home/Pictures/icon/light/nxinfo.png",
        "/home/Pictures/icon/light/nxmeminfo.png",
        "/home/Pictures/icon/light/nxperm.png",
        "/home/Pictures/icon/light/nxpower.png",
        "/home/Pictures/icon/light/nxreg.png",
        "/home/Pictures/icon/light/nxsettings.png",
        "/home/Pictures/icon/light/nxtop.png",
        "/home/Pictures/icon/light/doom.png",
        "/home/Pictures/icon/light/raptor.png",
        "/home/Pictures/icon/light/textedit.png",
        "/home/Pictures/icon/light/folder.png",
        "/home/Pictures/icon/light/file_c.png",
        "/home/Pictures/icon/light/file_text.png",
        "/home/Pictures/icon/light/file_config.png",
        "/home/Pictures/icon/light/file_html.png",
        "/home/Pictures/icon/light/file_script.png",
        "/home/Pictures/icon/light/file_image.png",
        "/home/Pictures/icon/light/file_video.png",
        "/home/Pictures/icon/light/file_binary.png",
        "/home/Pictures/icon/light/file_executable.png",
        "/home/Pictures/icon/light/file_generic.png",
        "/home/Pictures/icon/light/fallback.png",
    },
};

/* One cache slot per (theme, app id).  `tried` distinguishes "never asked"
 * from "asked and it's missing/bad" so a permanently-absent icon costs one
 * failed load for the whole process lifetime, not one per redraw. */
typedef struct {
  os1_image_t *img;
  int tried;
} nxicon_slot_t;

static nxicon_slot_t nxicon_cache[2][NXICON_COUNT];
static int nxicon_tile_size; /* size the cache was last built for */

/* nxicon_ci_prefix - case-insensitive prefix test: 1 if 's' starts with
 * 'prefix' ignoring ASCII case, 0 otherwise. This environment's os1.h only
 * declares strcasecmp() (whole-string), not a bounded/prefix strncasecmp(),
 * so this is a tiny self-contained helper in the same header-only style as
 * the rest of nxicon.h — no new libc dependency. */
static inline int nxicon_ci_prefix(const char *s, const char *prefix) {
  while (*prefix) {
    if (!*s)
      return 0;
    char a = *s, b = *prefix;
    if (a >= 'A' && a <= 'Z')
      a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z')
      b = (char)(b + 32);
    if (a != b)
      return 0;
    s++;
    prefix++;
  }
  return 1;
}

/*
 * nxicon_classify - map a window title to an NXICON_* id, for a RUNNING
 * window (nxui's dock). Use nxicon_classify_path() below for an app-table
 * entry's executable path (nxlauncher's grid) instead — a window title can
 * carry extra text a bare exe path never would (e.g. nxshell titles its
 * window "NXShell PID 6": different case than the binary name "nxshell",
 * plus a trailing " PID <n>"), so this is deliberately looser:
 *
 *   - strips any directory prefix first (some windows carry the full exe
 *     path as their title instead of a bare name);
 *   - matches case-INSENSITIVELY, since an app is free to title-case its
 *     own window ("NXShell") while its binary on disk is lowercase
 *     ("nxshell");
 *   - matches by PREFIX, not exact, so a trailing suffix ("NXShell PID 6",
 *     "nxfilem — folder view") doesn't defeat the match.
 *
 * Checked longest/most-specific prefix first where one binary name could
 * otherwise shadow another (none currently collide, but nxmemstat/nxsettings
 * etc. are kept in a stable, deliberate order rather than alphabetical).
 *
 * Anything unrecognised is NXICON_UNKNOWN — a valid icon choice, not a
 * failure.
 */
static inline int nxicon_classify(const char *title) {
  if (!title)
    return NXICON_UNKNOWN;
  const char *base = strrchr(title, '/');
  base = base ? base + 1 : title;
  if (nxicon_ci_prefix(base, "nxlauncher"))
    return NXICON_LAUNCHER;
  if (nxicon_ci_prefix(base, "nxshell"))
    return NXICON_SHELL;
  if (nxicon_ci_prefix(base, "nxfilem"))
    return NXICON_FILEM;
  if (nxicon_ci_prefix(base, "nximage"))
    return NXICON_IMAGE;
  if (nxicon_ci_prefix(base, "nxinfo"))
    return NXICON_INFO;
  if (nxicon_ci_prefix(base, "nxmemstat"))
    return NXICON_MEMSTAT;
  if (nxicon_ci_prefix(base, "nxperm"))
    return NXICON_PERM;
  if (nxicon_ci_prefix(base, "nxpower"))
    return NXICON_POWER;
  if (nxicon_ci_prefix(base, "nxreg"))
    return NXICON_REG;
  if (nxicon_ci_prefix(base, "nxsettings"))
    return NXICON_SETTINGS;
  if (nxicon_ci_prefix(base, "nxtop"))
    return NXICON_TOP;
  if (nxicon_ci_prefix(base, "doom"))
    return NXICON_DOOM;
  if (nxicon_ci_prefix(base, "raptor"))
    return NXICON_RAPTOR;
  if (nxicon_ci_prefix(base, "kilo"))
    return NXICON_TEXTEDIT;
  return NXICON_UNKNOWN;
}

/*
 * nxicon_classify_path - map an app-table executable path (e.g.
 * "/sys/bin/nxshell") to an NXICON_* id by an EXACT basename match.
 * Anything unrecognised is NXICON_UNKNOWN (the generic app icon).
 */
static inline int nxicon_classify_path(const char *path) {
  if (!path)
    return NXICON_UNKNOWN;
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  if (strcmp(base, "nxlauncher") == 0)
    return NXICON_LAUNCHER;
  if (strcmp(base, "nxshell") == 0)
    return NXICON_SHELL;
  if (strcmp(base, "nxfilem") == 0)
    return NXICON_FILEM;
  if (strcmp(base, "nximage") == 0)
    return NXICON_IMAGE;
  if (strcmp(base, "nxinfo") == 0)
    return NXICON_INFO;
  if (strcmp(base, "nxmemstat") == 0)
    return NXICON_MEMSTAT;
  if (strcmp(base, "nxperm") == 0)
    return NXICON_PERM;
  if (strcmp(base, "nxpower") == 0)
    return NXICON_POWER;
  if (strcmp(base, "nxreg") == 0)
    return NXICON_REG;
  if (strcmp(base, "nxsettings") == 0)
    return NXICON_SETTINGS;
  if (strcmp(base, "nxtop") == 0)
    return NXICON_TOP;
  if (strcmp(base, "doom") == 0)
    return NXICON_DOOM;
  if (strcmp(base, "raptor") == 0)
    return NXICON_RAPTOR;
  if (strcmp(base, "kilo") == 0)
    return NXICON_TEXTEDIT;
  return NXICON_UNKNOWN;
}

/*
 * nxicon_classify_file - map a filesystem entry (name + is_dir) to an
 * NXICON_* id for nxfilem's content list.  Directories always get
 * NXICON_FOLDER regardless of name.  Files are classified by extension
 * (case-insensitive, via os1_image_ext_eq from image.h — same helper
 * os1_image_path_has_known_ext already uses), falling back to
 * NXICON_FILE_GENERIC for anything unrecognised.  Kept in the same
 * header-only, static-inline style as nxicon_classify()/nxicon_classify_path()
 * above; deliberately independent of nxassoc.h (icon choice and launch
 * association are separate concerns that happen to share extension lists).
 */
static inline int nxicon_classify_file(const char *name, int is_dir) {
  if (is_dir)
    return NXICON_FOLDER;
  if (!name)
    return NXICON_FILE_GENERIC;
  const char *ext = strrchr(name, '.');
  if (!ext || ext == name)
    return NXICON_FILE_GENERIC;

  static const char *const c_exts[] = {".c", ".h", ".cpp", ".cc", ".hpp",
                                       ".s", NULL};
  static const char *const text_exts[] = {".txt", ".md",  ".log",
                                          ".rst", ".csv", NULL};
  static const char *const cfg_exts[] = {".cfg", ".json", ".ini",
                                         ".toml", ".yaml", ".yml", NULL};
  static const char *const html_exts[] = {".html", ".htm", ".css", NULL};
  static const char *const script_exts[] = {".lua", ".sh", ".py", NULL};
  static const char *const video_exts[] = {".mp4", ".mkv", ".avi",
                                           ".webm", NULL};
  static const char *const bin_exts[] = {".bin", ".dat", ".o", ".elf", NULL};

  if (os1_image_path_has_known_ext(name))
    return NXICON_FILE_IMAGE;
  for (int i = 0; c_exts[i]; i++)
    if (os1_image_ext_eq(ext, c_exts[i]))
      return NXICON_FILE_C;
  for (int i = 0; script_exts[i]; i++)
    if (os1_image_ext_eq(ext, script_exts[i]))
      return NXICON_FILE_SCRIPT;
  for (int i = 0; html_exts[i]; i++)
    if (os1_image_ext_eq(ext, html_exts[i]))
      return NXICON_FILE_HTML;
  for (int i = 0; cfg_exts[i]; i++)
    if (os1_image_ext_eq(ext, cfg_exts[i]))
      return NXICON_FILE_CONFIG;
  for (int i = 0; text_exts[i]; i++)
    if (os1_image_ext_eq(ext, text_exts[i]))
      return NXICON_FILE_TEXT;
  for (int i = 0; video_exts[i]; i++)
    if (os1_image_ext_eq(ext, video_exts[i]))
      return NXICON_FILE_VIDEO;
  if (os1_image_ext_eq(ext, ".elf"))
    return NXICON_FILE_EXECUTABLE;
  for (int i = 0; bin_exts[i]; i++)
    if (os1_image_ext_eq(ext, bin_exts[i]))
      return NXICON_FILE_BINARY;
  return NXICON_FILE_GENERIC;
}

/* nxicon_reset_cache - drop every cached scaled icon and retry flag (both
 * themes).  Only needed if the tile size itself changes (a future DPI/zoom
 * feature); a plain theme flip does NOT need this, since both themes are
 * cached side by side already. */
static inline void nxicon_reset_cache(void) {
  for (int t = 0; t < 2; t++)
    for (int i = 0; i < NXICON_COUNT; i++) {
      if (nxicon_cache[t][i].img)
        os1_image_free(nxicon_cache[t][i].img);
      nxicon_cache[t][i].img = NULL;
      nxicon_cache[t][i].tried = 0;
    }
  nxicon_tile_size = 0;
}

/*
 * nxicon_get - cached tile_size x tile_size ARGB icon for (app_id, light), or
 * NULL if icons are disabled or this asset is missing/undecodable.  Callers
 * always treat NULL as "fall back to the classic flat-color rounded tile" —
 * see the header comment above.
 */
static inline os1_image_t *nxicon_get(int app_id, int light, int tile_size) {
#if !NXICON_ENABLE
  (void)app_id;
  (void)light;
  (void)tile_size;
  return NULL;
#else
  if (app_id < 0 || app_id >= NXICON_COUNT || tile_size <= 0)
    return NULL;

  if (nxicon_tile_size != tile_size)
    nxicon_reset_cache(); /* size changed since the cache was built: rebuild
                           * lazily, one icon at a time, as each is next
                           * requested — not an eager reload of all fifteen. */
  nxicon_tile_size = tile_size;

  int theme = light ? NXICON_THEME_LIGHT : NXICON_THEME_DARK;
  nxicon_slot_t *slot = &nxicon_cache[theme][app_id];
  if (slot->img)
    return slot->img;
  if (slot->tried)
    return NULL;
  slot->tried = 1;

  os1_image_t *src = os1_image_load(nxicon_paths[theme][app_id]);
  if (!src)
    return NULL;

  /* Trusted-asset fast path: known-square OS icon, so resample straight to
   * the tile size instead of nximage.c's aspect-fit dance for arbitrary user
   * photos.  Skip the resample entirely if the asset already matches. */
  os1_image_t *scaled = (src->w == tile_size && src->h == tile_size)
                            ? src
                            : os1_image_resample(src, tile_size, tile_size);
  if (scaled != src)
    os1_image_free(src);
  if (!scaled)
    return NULL;

  slot->img = scaled;
  return slot->img;
#endif
}

#endif /* _USER_NXICON_H */