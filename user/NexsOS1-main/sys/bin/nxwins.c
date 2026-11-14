/*
 * nxwins — inspect and control existing WINDOW/SURFACE object geometry.
 *
 * This utility adds no window policy. Every operation is a thin client of the
 * capability-gated window/surface interfaces already used by nxmoused and the
 * user compositor.
 */
#include <os1.h>

static int parse_int(const char *text, int *value) {
  if (text == NULL || value == NULL || text[0] == '\0')
    return -EINVAL;
  int negative = text[0] == '-';
  int offset = negative ? 1 : 0;
  uint64_t parsed = 0U;
  uint64_t limit = negative ? UINT64_C(2147483648) : UINT64_C(2147483647);
  if (text[offset] == '\0')
    return -EINVAL;
  for (int i = 0; i < 11; ++i) {
    char c = text[offset + i];
    if (c == '\0') {
      int64_t signed_value =
          negative ? -(int64_t)parsed : (int64_t)parsed;
      *value = (int)signed_value;
      return 0;
    }
    if (c < '0' || c > '9')
      return -EINVAL;
    uint64_t digit = (uint64_t)(c - '0');
    if (parsed > (limit - digit) / UINT64_C(10))
      return -ERANGE;
    parsed = parsed * UINT64_C(10) + digit;
  }
  return -ERANGE;
}

static int snapshot_window(int id, struct os1_window_snapshot_v1 *snapshot) {
  if (id <= 0 || snapshot == NULL)
    return -EINVAL;
  *snapshot = (struct os1_window_snapshot_v1){
      .abi_version = OS1_COMPOSITOR_ABI_V1,
      .struct_size = sizeof(*snapshot),
      .id = id};
  return (int)OS1_window_snapshot(snapshot);
}

static const char *window_state(unsigned flags) {
  if (flags & WININFO_MINIMIZED) return "min";
  if (flags & WININFO_FOCUSED) return "focus";
  if (flags & WININFO_VISIBLE) return "shown";
  return "hidden";
}

static void print_snapshot(const struct os1_window_snapshot_v1 *snapshot,
                           const char *label) {
  printf("%d  %d  %-6s  pos=%d,%d  surface=%dx%d  draw=%dx%d  %s\n",
         snapshot->id, snapshot->pid, window_state(snapshot->flags),
         snapshot->x, snapshot->y, snapshot->width, snapshot->height,
         snapshot->draw_width, snapshot->draw_height, label);
}

static int cmd_list(void) {
  struct window_info windows[32];
  long count = OS1_window_enum(windows, 32);
  if (count < 0) {
    printf("nxwins: window enumeration failed (%ld)\n", count);
    return 1;
  }
  printf("ID  PID  STATE   GEOMETRY                              NAME\n");
  for (int i = 0; i < 32 && (long)i < count; ++i) {
    struct os1_window_snapshot_v1 snapshot;
    if (snapshot_window(windows[i].id, &snapshot) != 0)
      continue;
    char idkey[48], idname[40];
    const char *label = windows[i].title;
    snprintf(idkey, sizeof(idkey), "sys.proc.%d.name", windows[i].pid);
    if (OS1_registry_get(idkey, idname, sizeof(idname)) == 0 && idname[0])
      label = idname;
    print_snapshot(&snapshot, label);
  }
  return 0;
}

static int cmd_inspect(const char *id_arg) {
  int id = 0;
  if (parse_int(id_arg, &id) != 0 || id <= 0)
    return 1;
  struct os1_window_snapshot_v1 snapshot;
  int result = snapshot_window(id, &snapshot);
  if (result != 0) {
    printf("nxwins: inspect %d failed (%d)\n", id, result);
    return 1;
  }
  print_snapshot(&snapshot, snapshot.title);
  return 0;
}

static int cmd_focus(const char *id_arg) {
  int id = 0;
  if (parse_int(id_arg, &id) != 0 || id <= 0)
    return 1;
  int result = OS1_window_focus(id);
  if (result == 0) {
    printf("nxwins: focused window %d\n", id);
    return 0;
  }
  printf("nxwins: focus %d failed (%d)\n", id, result);
  return 1;
}

static int cmd_move(const char *id_arg, const char *x_arg, const char *y_arg) {
  int id = 0, x = 0, y = 0;
  if (parse_int(id_arg, &id) != 0 || parse_int(x_arg, &x) != 0 ||
      parse_int(y_arg, &y) != 0 || id <= 0)
    return 1;
  long result = OS1_window_move(id, x, y);
  if (result == 0) {
    printf("nxwins: window %d moved to %d,%d\n", id, x, y);
    return 0;
  }
  printf("nxwins: move %d failed (%ld)\n", id, result);
  return 1;
}

static int parse_size(const char *w_arg, const char *h_arg, int *w, int *h) {
  if (parse_int(w_arg, w) != 0 || parse_int(h_arg, h) != 0 ||
      *w < 1 || *h < 1 || *w > 4096 || *h > 4096)
    return -EINVAL;
  return 0;
}

static int cmd_resize(const char *id_arg, const char *w_arg,
                      const char *h_arg, int presentation_only) {
  int id = 0, width = 0, height = 0;
  if (parse_int(id_arg, &id) != 0 || id <= 0 ||
      parse_size(w_arg, h_arg, &width, &height) != 0)
    return 1;
  int result = presentation_only
                   ? OS1_window_present_resize(id, width, height)
                   : OS1_window_resize(id, width, height);
  if (result == 0) {
    printf("nxwins: window %d %s -> %dx%d\n", id,
           presentation_only ? "draw geometry" : "logical surface",
           width, height);
    return 0;
  }
  printf("nxwins: %s resize %d failed (%d)\n",
         presentation_only ? "presentation" : "logical", id, result);
  return 1;
}

static void usage(void) {
  printf("usage: nxwins\n");
  printf("       nxwins inspect <id> | focus <id>\n");
  printf("       nxwins move <id> <x> <y>\n");
  printf("       nxwins resize <id> <w> <h>     # logical surface\n");
  printf("       nxwins present <id> <w> <h>    # presentation only\n");
}

int main(int argc, char **argv) {
  if (argc < 2)
    return cmd_list();
  if (argc == 3 && strncmp(argv[1], "inspect", 8) == 0)
    return cmd_inspect(argv[2]);
  if (argc == 3 && strncmp(argv[1], "focus", 6) == 0)
    return cmd_focus(argv[2]);
  if (argc == 5 && strncmp(argv[1], "move", 5) == 0)
    return cmd_move(argv[2], argv[3], argv[4]);
  if (argc == 5 && strncmp(argv[1], "resize", 7) == 0)
    return cmd_resize(argv[2], argv[3], argv[4], 0);
  if (argc == 5 && strncmp(argv[1], "present", 8) == 0)
    return cmd_resize(argv[2], argv[3], argv[4], 1);
  usage();
  return 1;
}
