/*
 * nxres — display/output and desktop-scale control.
 *
 * Hardware modes come exclusively from the display provider catalog. The
 * logical desktop scale is independent from the physical scanout. Style,
 * theme and background continue to use the shared nxres policy helpers.
 */
#include <os1.h>
#include <os1_surface.h>

#include <nxres.h>

static int parse_uint_arg(const char *text, int maximum, int *value) {
  if (text == NULL || value == NULL || text[0] == '\0' || maximum < 0)
    return -EINVAL;
  int result = 0;
  for (int i = 0; i < 10; ++i) {
    char c = text[i];
    if (c == '\0') {
      *value = result;
      return 0;
    }
    if (c < '0' || c > '9')
      return -EINVAL;
    int digit = c - '0';
    if (digit > maximum || result > (maximum - digit) / 10)
      return -ERANGE;
    result = result * 10 + digit;
  }
  return -ERANGE;
}

static void print_mode_line(uint32_t index,
                            const struct os1_display_mode_v1 *mode) {
  const char *current =
      (mode->flags & OS1_DISPLAY_MODE_CURRENT) ? " current" : "";
  const char *preferred =
      (mode->flags & OS1_DISPLAY_MODE_PREFERRED) ? " preferred" : "";
  const char *interlaced =
      (mode->flags & OS1_DISPLAY_MODE_INTERLACED) ? " interlaced" : "";
  if (mode->refresh_millihz != 0U) {
    printf("  %u: %ux%u @ %u.%03u Hz%s%s%s\n", index, mode->width,
           mode->height, mode->refresh_millihz / 1000U,
           mode->refresh_millihz % 1000U, current, preferred, interlaced);
  } else {
    printf("  %u: %ux%u%s%s%s\n", index, mode->width, mode->height, current,
           preferred, interlaced);
  }
}

static int query_catalog_head(struct os1_display_mode_v1 *mode) {
  int result = (int)OS1_display_mode_query(0U, mode);
  if (result != 0)
    OS1_report_error("nxres.mode_catalog", result);
  return result;
}

static int print_modes(void) {
  struct os1_display_mode_v1 mode;
  if (query_catalog_head(&mode) != 0)
    return 1;
  uint32_t count = mode.count;
  if (count > OS1_DISPLAY_MODE_MAX)
    count = OS1_DISPLAY_MODE_MAX;
  printf("nxres: hardware modes, generation %llu\n",
         (unsigned long long)mode.generation);
  print_mode_line(0U, &mode);
  for (uint32_t i = 1U; i < count; ++i) {
    if (OS1_display_mode_query(i, &mode) != 0)
      return 1;
    print_mode_line(i, &mode);
  }
  return 0;
}

static int set_mode_index(uint32_t index) {
  if (index >= OS1_DISPLAY_MODE_MAX)
    return 1;
  struct os1_display_mode_v1 mode;
  int queried = (int)OS1_display_mode_query(index, &mode);
  if (queried != 0) {
    printf("nxres: unsupported mode index %u\n", index);
    return 1;
  }
  int changed = (int)OS1_display_mode_set(index, mode.generation);
  if (changed != 0) {
    printf("nxres: mode %u rejected (%d)\n", index, changed);
    return 1;
  }
  printf("nxres: hardware mode -> %ux%u", mode.width, mode.height);
  if (mode.refresh_millihz != 0U)
    printf(" @ %u.%03u Hz", mode.refresh_millihz / 1000U,
           mode.refresh_millihz % 1000U);
  printf("\n");
  return 0;
}

static int find_mode(int width, int height, uint32_t *index) {
  struct os1_display_mode_v1 mode;
  if (index == NULL || query_catalog_head(&mode) != 0)
    return -1;
  uint32_t count = mode.count;
  if (count > OS1_DISPLAY_MODE_MAX)
    count = OS1_DISPLAY_MODE_MAX;
  uint32_t candidate = UINT32_MAX;
  for (uint32_t i = 0U; i < count; ++i) {
    if (i != 0U && OS1_display_mode_query(i, &mode) != 0)
      return -1;
    if (mode.width != (uint32_t)width || mode.height != (uint32_t)height)
      continue;
    candidate = i;
    if (mode.flags & (OS1_DISPLAY_MODE_CURRENT | OS1_DISPLAY_MODE_PREFERRED))
      break;
  }
  if (candidate == UINT32_MAX)
    return -1;
  *index = candidate;
  return 0;
}

static void print_status(void) {
  int width = 0, height = 0;
  if (OS1_display_get_size(&width, &height) == 0)
    printf("nxres: logical desktop %dx%d\n", width, height);
  struct os1_display_mode_v1 mode;
  if (query_catalog_head(&mode) == 0) {
    uint32_t count = mode.count > OS1_DISPLAY_MODE_MAX
                         ? OS1_DISPLAY_MODE_MAX
                         : mode.count;
    for (uint32_t i = 0U; i < count; ++i) {
      if (i != 0U && OS1_display_mode_query(i, &mode) != 0)
        break;
      if (mode.flags & OS1_DISPLAY_MODE_CURRENT) {
        printf("       hardware      %ux%u", mode.width, mode.height);
        if (mode.refresh_millihz != 0U)
          printf(" @ %u.%03u Hz", mode.refresh_millihz / 1000U,
                 mode.refresh_millihz % 1000U);
        printf("\n");
        break;
      }
    }
  }

  char buf[32] = {0};
  printf("       style         %s\n",
         nxres_get_style(buf, sizeof(buf)) == 0 ? buf : "unknown");
  printf("       theme         %s\n",
         nxres_get_theme(buf, sizeof(buf)) == 0 ? buf : "unknown");
  printf("       background    %s\n",
         nxres_get_background(buf, sizeof(buf)) == 0 ? buf : "unknown");
}

static int set_named_property(const char *kind, const char *value) {
  int result = -1;
  if (strncmp(kind, "style", 6) == 0)
    result = nxres_set_style(value);
  else if (strncmp(kind, "theme", 6) == 0)
    result = nxres_set_theme(value);
  else if (strncmp(kind, "bg", 3) == 0)
    result = nxres_set_background(value);
  else
    return -2;
  if (result == 0) {
    printf("nxres: %s -> %s\n", kind, value);
    return 0;
  }
  printf("nxres: %s '%s' rejected\n", kind, value);
  return 1;
}

static void print_usage(void) {
  printf("usage: nxres | nxres modes | nxres mode <index> | nxres <w> <h>\n");
  printf("       nxres zoom <pct> | nxres style|theme|bg <name>\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    print_status();
    print_usage();
    return 0;
  }

  if (strncmp(argv[1], "modes", 6) == 0)
    return print_modes();

  if (argc >= 3 && strncmp(argv[1], "mode", 5) == 0) {
    int index = 0;
    if (parse_uint_arg(argv[2], (int)OS1_DISPLAY_MODE_MAX - 1, &index) == 0)
      return set_mode_index((uint32_t)index);
  }

  if (argc >= 3 && strncmp(argv[1], "zoom", 5) == 0) {
    int percent = 0;
    if (parse_uint_arg(argv[2], OS1_DISPLAY_ZOOM_MAX_PERCENT, &percent) != 0)
      return 1;
    int result = OS1_display_set_zoom(percent);
    if (result == 0) {
      printf("nxres: UI scale -> %d%%\n", percent);
      return 0;
    }
    printf("nxres: UI scale rejected (%d)\n", result);
    return 1;
  }

  if (argc >= 3) {
    int named = set_named_property(argv[1], argv[2]);
    if (named != -2)
      return named;
  }

  if (argc >= 3) {
    int width = 0, height = 0;
    uint32_t index = 0U;
    if (parse_uint_arg(argv[1], 8192, &width) != 0 ||
        parse_uint_arg(argv[2], 8192, &height) != 0 || width <= 0 ||
        height <= 0 || find_mode(width, height, &index) != 0) {
      printf("nxres: %s x %s is not advertised by the display provider\n",
             argv[1], argv[2]);
      return 1;
    }
    return set_mode_index(index);
  }

  print_usage();
  return 1;
}
