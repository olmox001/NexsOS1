/* Runtime layout configuration via the existing authorized registry API.
 * Does not change input protocol, keyboard focus or kernel driver behavior. */
#include <os1.h>
#include <string.h>
#include <nxkeyboard_layout.h>

/* Same raw ELF argument-vector layout used by nxttyd, without a char**
 * API dependency in the Power12-enforced userland control module. */
struct nxkeymap_argv {
  char *value[16];
};

int main(int argc, const struct nxkeymap_argv *argv) {
  if (argv == NULL) return 2;
  if (argc == 1) {
    char current[24] = {0};
    int rc = OS1_registry_get(NXS_KBD_REGISTRY_KEY, current,
                               sizeof(current));
    if (rc != 0) return 1;
    return printf("keyboard layout: %s\n", current) < 0 ? 1 : 0;
  }
  if (argc != 2) {
    print("usage: nxkeymap [mac19ita|us]\n");
    return 2;
  }
  int layout = nxs_keyboard_layout_id(argv->value[1]);
  if (layout == 0) {
    print("nxkeymap: unsupported layout\n");
    return 2;
  }
  const char *name = layout == NXS_KBD_US ? "us" : NXS_KBD_DEFAULT;
  int rc = OS1_registry_set(NXS_KBD_REGISTRY_KEY, name);
  if (rc != 0) {
    OS1_report_error("nxkeymap.permission", rc);
    print("nxkeymap: permission denied or registry unavailable\n");
    return 1;
  }
  return printf("keyboard layout: %s\n", name) < 0 ? 1 : 0;
}
