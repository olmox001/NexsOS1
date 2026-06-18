/*
 * user/bin/nxres.c
 * NEXS display + compositor-look tool (GFX-DYN-01 / DIR-07).
 *
 * Usage:
 *   nxres                  print the current desktop resolution
 *   nxres <width> <height> set the desktop resolution at runtime
 *   nxres style <name>     classic | material | glass | minimal | retro
 *   nxres theme <name>     light | dark
 *
 * Exercises the dynamic-display chain from userland: SYS_SET_DISPLAY_MODE ->
 * gpu_set_mode() (virtio-gpu recreates the scanout) -> compositor_resize().
 * Style/theme go through SYS_SET_STYLE -> compositor_set_style/theme().
 */
#include <os1.h>

/* Style ids must match enum in kernel/include/kernel/compositor_style.h. */
static const char *style_names[] = {"classic", "material", "glass", "minimal",
                                    "retro"};
static const char *theme_names[] = {"light", "dark"};

static int name_to_id(const char *name, const char *const *list, int n) {
  for (int i = 0; i < n; i++) {
    /* exact match: equal up to and including the NUL */
    if (strncmp(name, list[i], 32) == 0)
      return i;
  }
  return -1;
}

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
    long info = _sys_display_info();
    int w = (int)((info >> 16) & 0xFFFF);
    int h = (int)(info & 0xFFFF);
    printf("nxres: current desktop %dx%d\n", w, h);
    printf("usage: nxres <w> <h> | nxres zoom <pct> | nxres style|theme <name>\n");
    return 0;
  }

  /* nxres style <name> */
  if (argc >= 3 && strncmp(argv[1], "style", 6) == 0) {
    int sid = name_to_id(argv[2], style_names, 5);
    if (sid < 0) {
      printf("nxres: unknown style '%s'\n", argv[2]);
      return 1;
    }
    if (_sys_set_style(sid, -1) == 0) {
      printf("nxres: style -> %s\n", argv[2]);
      return 0;
    }
    printf("nxres: set_style failed\n");
    return 1;
  }

  /* nxres zoom <percent> */
  if (argc >= 3 && strncmp(argv[1], "zoom", 5) == 0) {
    int p = atoi(argv[2]);
    if (_sys_set_zoom(p) == 0) {
      printf("nxres: zoom -> %d%%\n", p);
      return 0;
    }
    printf("nxres: set_zoom failed\n");
    return 1;
  }

  /* nxres theme <name> */
  if (argc >= 3 && strncmp(argv[1], "theme", 6) == 0) {
    int tid = name_to_id(argv[2], theme_names, 2);
    if (tid < 0) {
      printf("nxres: unknown theme '%s'\n", argv[2]);
      return 1;
    }
    if (_sys_set_style(-1, tid) == 0) {
      printf("nxres: theme -> %s\n", argv[2]);
      return 0;
    }
    printf("nxres: set_theme failed\n");
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
    int r = _sys_set_display_mode(w, h);
    if (r == 0) {
      printf("nxres: display set to %dx%d\n", w, h);
      return 0;
    }
    printf("nxres: failed to set %dx%d (err %d)\n", w, h, r);
    return 1;
  }

  printf("nxres: bad arguments\n");
  printf("usage: nxres <w> <h> | nxres zoom <pct> | nxres style|theme <name>\n");
  return 1;
}
