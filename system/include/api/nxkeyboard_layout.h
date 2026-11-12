/* NexsOS input scheme policy: C11, host-testable, no kernel imports.
 * The historical keyboard provider had US and Italian schemes; the
 * 2026-11-08 migration accidentally removed selection and applied Italian
 * accented-key overrides unconditionally. The physical scancode is separate
 * from the selected interpretation, so GUI apps still receive the same IPC. */
#ifndef NXS_KEYBOARD_LAYOUT_H
#define NXS_KEYBOARD_LAYOUT_H
#include <stddef.h>

#define NXS_KBD_MAC19ITA 1
#define NXS_KBD_US 2
#define NXS_KBD_REGISTRY_KEY "keyboard.layout"
#define NXS_KBD_DEFAULT "mac19ita"

int nxs_keyboard_layout_id(const char *name);
const char *nxs_keyboard_utf8(int layout, unsigned code, int shift);
struct nxs_keyboard_state {
  unsigned shift_keys, ctrl_keys;
  int caps;
};
struct nxs_keyboard_key {
  unsigned code;
  int state;
  unsigned char key;
  char utf8[4];
};
/* 1 routed key, 0 modifier/ignored input, negative invalid argument. */
int nxs_keyboard_translate(struct nxs_keyboard_state *state, unsigned code,
                            int value, int layout,
                            struct nxs_keyboard_key *out);
#endif
