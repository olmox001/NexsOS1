/* Host contract tests against the REAL userland keyboard policy. */
#include <nxkeyboard_layout.h>
#include <stdio.h>
#include <string.h>

static int test_us_is_unmodified(void) {
  if (os1_keyboard_layout_id("us") != NEXSOS1_KBD_US) return 1;
  const unsigned keys[] = {26U,39U,40U,41U,43U};
  for (unsigned i=0U;i<sizeof(keys)/sizeof(keys[0]);++i) {
    if (os1_keyboard_layout_utf8(NEXSOS1_KBD_US,keys[i],0) != NULL ||
        os1_keyboard_layout_utf8(NEXSOS1_KBD_US,keys[i],1) != NULL) return 2;
  }
  return 0;
}
static int test_it_preserves_accent(void) {
  if (os1_keyboard_layout_id("mac19ita") != NEXSOS1_KBD_MAC19ITA) return 1;
  if (strcmp(os1_keyboard_layout_utf8(NEXSOS1_KBD_MAC19ITA,39U,0),"\xC3\xB2") != 0)
    return 2;
  if (strcmp(os1_keyboard_layout_utf8(NEXSOS1_KBD_MAC19ITA,40U,1),"\xC3\x80") != 0)
    return 3;
  if (os1_keyboard_layout_utf8(NEXSOS1_KBD_MAC19ITA,39U,1) != NULL) return 4;
  return 0;
}
static int test_unknown_layout_fails_closed(void) {
  if (os1_keyboard_layout_id("spoofed") != 0) return 1;
  if (os1_keyboard_layout_id(NULL) != NEXSOS1_KBD_MAC19ITA) return 2;
  if (os1_keyboard_layout_utf8(0,39U,0) != NULL) return 3;
  return 0;
}
static int test_modifier_pairs_and_repeat(void) {
  struct os1_keyboard_state state = {0};
  struct os1_keyboard_key key;
  if (os1_keyboard_translate(&state, 42U, 1, NEXSOS1_KBD_US, &key) != 0 ||
      os1_keyboard_translate(&state, 54U, 1, NEXSOS1_KBD_US, &key) != 0 ||
      os1_keyboard_translate(&state, 42U, 0, NEXSOS1_KBD_US, &key) != 0) return 1;
  if (os1_keyboard_translate(&state, 30U, 1, NEXSOS1_KBD_US, &key) != 1 ||
      key.key != 'A') return 2;
  if (os1_keyboard_translate(&state, 54U, 0, NEXSOS1_KBD_US, &key) != 0 ||
      os1_keyboard_translate(&state, 58U, 1, NEXSOS1_KBD_US, &key) != 0 ||
      os1_keyboard_translate(&state, 58U, 2, NEXSOS1_KBD_US, &key) != 0) return 3;
  if (os1_keyboard_translate(&state, 30U, 1, NEXSOS1_KBD_US, &key) != 1 ||
      key.key != 'A') return 4;
  if (os1_keyboard_translate(&state, 29U, 1, NEXSOS1_KBD_US, &key) != 0 ||
      os1_keyboard_translate(&state, 97U, 1, NEXSOS1_KBD_US, &key) != 0 ||
      os1_keyboard_translate(&state, 29U, 0, NEXSOS1_KBD_US, &key) != 0) return 5;
  if (os1_keyboard_translate(&state, 30U, 1, NEXSOS1_KBD_US, &key) != 1 ||
      key.key != 1U || key.utf8[0] != 1) return 6;
  return 0;
}

int main(void) {
  int a=test_us_is_unmodified(),b=test_it_preserves_accent();
  int c=test_unknown_layout_fails_closed();
  int d = test_modifier_pairs_and_repeat();
  if (a || b || c || d) {
    int written=printf("keyboard layout tests failed: %d %d %d %d\n",a,b,c,d);
    return written > 0 ? 1 : 2;
  }
  return 0;
}
