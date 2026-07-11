/*
 * user/sys/bin/nxres.c
 * NEXS display + compositor-look tool (GFX-DYN-01 / DIR-07).
 *
 * Usage:
 *   nxres                  print the current desktop resolution + applied
 *                          style/theme/background
 *   nxres <width> <height> set the desktop resolution at runtime
 *   nxres style <name>     nexs | classic | material | glass | minimal | retro
 *   nxres theme <name>     light | dark
 *   nxres bg <name>        black | red | green | yellow | blue | magenta |
 *                          cyan | white | gray | bred | … | bwhite
 *                          ('blue' is the default nexs gradient)
 *
 * Exercises the dynamic-display chain from userland: SYS_SET_DISPLAY_MODE ->
 * gpu_set_mode() (virtio-gpu recreates the scanout) -> compositor_resize().
 *
 * The actual style/theme/bg get/set logic (name tables, id lookup, the
 * SYS_SET_STYLE call, the registry mirror) lives in nxres.h so every other
 * app (nxsettings, nxbar, nxntfy_srv, nxui, nxlauncher, ...) shares this
 * exact implementation instead of re-deriving it — this file is now just
 * the CLI/stdout wrapper around it.
 */
#include <os1.h>

#include "nxres.h"

static int is_digit_str(const char *s) {
  if (!s || !*s)
    return 0;
  for (const char *p = s; *p; p++)
    if (*p < '0' || *p > '9')
      return 0;
  return 1;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    /* Show current desktop resolution and the applied style/theme/bg
     * (single canonical getter: nxres_get_* in nxres.h). */
    long info = OS1_display_info();
    int w = (int)((info >> 16) & 0xFFFF);
    int h = (int)(info & 0xFFFF);
    printf("nxres: current desktop %dx%d\n", w, h);

    char buf[32] = {0};
    if (nxres_get_style(buf, sizeof(buf)) == 0)
      printf("       style       %s\n", buf);
    else
      printf("       style       unknown\n");

    if (nxres_get_theme(buf, sizeof(buf)) == 0)
      printf("       theme       %s\n", buf);
    else
      printf("       theme       unknown\n");

    if (nxres_get_background(buf, sizeof(buf)) == 0)
      printf("       background  %s\n", buf);
    else
      printf("       background  unknown\n");

    printf("\nusage: nxres <w> <h> | nxres zoom <pct> | nxres style|theme|bg "
           "<name>\n");
    return 0;
  }

  /* nxres style <name> */
  if (argc >= 3 && strncmp(argv[1], "style", 6) == 0) {
    int r = nxres_set_style(argv[2]);
    if (r == -1) {
      printf("nxres: unknown style '%s'\n", argv[2]);
      return 1;
    }
    if (r == 0) {
      printf("nxres: style -> %s\n", argv[2]);
      return 0;
    }
    printf("nxres: set_style failed\n");
    return 1;
  }

  /* nxres zoom <percent> */
  if (argc >= 3 && strncmp(argv[1], "zoom", 5) == 0) {
    int p = atoi(argv[2]);
    if (OS1_display_set_zoom(p) == 0) {
      printf("nxres: zoom -> %d%%\n", p);
      return 0;
    }
    printf("nxres: set_zoom failed\n");
    return 1;
  }

  /* nxres theme <name> */
  if (argc >= 3 && strncmp(argv[1], "theme", 6) == 0) {
    int r = nxres_set_theme(argv[2]);
    if (r == -1) {
      printf("nxres: unknown theme '%s'\n", argv[2]);
      return 1;
    }
    if (r == 0) {
      printf("nxres: theme -> %s\n", argv[2]);
      return 0;
    }
    printf("nxres: set_theme failed\n");
    return 1;
  }

  /* nxres bg <name> */
  if (argc >= 3 && strncmp(argv[1], "bg", 2) == 0) {
    int r = nxres_set_background(argv[2]);
    if (r == -1) {
      printf("nxres: unknown background '%s'\n", argv[2]);
      return 1;
    }
    if (r == 0) {
      printf("nxres: background -> %s\n", argv[2]);
      return 0;
    }
    printf("nxres: set_background failed\n");
    return 1;
  }

  /* nxres <w> <h> */
  if (argc >= 3 && is_digit_str(argv[1]) && is_digit_str(argv[2])) {
    int w = atoi(argv[1]);
    int h = atoi(argv[2]);
    if (w <= 0 || h <= 0) {
      printf("nxres: invalid size '%s x %s'\n", argv[1], argv[2]);
      return 1;
    }
    int r = OS1_display_set_mode(w, h);
    if (r == 0) {
      printf("nxres: display set to %dx%d\n", w, h);
      return 0;
    }
    printf("nxres: failed to set %dx%d (err %d)\n", w, h, r);
    return 1;
  }

  printf("nxres: bad arguments\n");
  printf("usage: nxres <w> <h> | nxres zoom <pct> | nxres style|theme|bg "
         "<name>\n");
  return 1;
}
