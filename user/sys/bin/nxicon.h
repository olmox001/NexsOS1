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
#define NXICON_UNKNOWN 14
#define NXICON_COUNT 15
/* Still not wired: nxscript.png (no program named nxscript yet — likely
 * meant for a future scripting app, not the same thing as file_script.png),
 * folder.png, and the per-filetype icons (file_binary, file_c, file_config,
 * file_executable, file_generic, file_html, file_image, file_script,
 * file_text, file_video). Those are for nxfilem's folder view (by extension,
 * not by app id) once that lands, not per-app tiles, so they stay out of
 * nxicon_paths for now. */

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