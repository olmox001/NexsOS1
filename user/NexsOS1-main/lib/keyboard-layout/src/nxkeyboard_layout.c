/* Shared keyboard interpretation; physical input routing belongs to nxkeyboardd. */
#include <nxkeyboard_layout.h>
#include <string.h>

int os1_keyboard_layout_default(void) { return NEXSOS1_KBD_MAC19ITA; }

int os1_keyboard_layout_id(const char *name) {
  if (name == NULL) return NEXSOS1_KBD_MAC19ITA;
  if (strcmp(name, "us") == 0) return NEXSOS1_KBD_US;
  if (strcmp(name, NEXSOS1_KBD_DEFAULT) == 0 ||
      strcmp(name, "it") == 0) return NEXSOS1_KBD_MAC19ITA;
  return 0;
}

/* Preserve the real historical IT UTF-8 layout, including Shift variants.
 * US physical scancodes never inherit these overrides. */
const char *os1_keyboard_layout_utf8(int layout, unsigned code,
                                           int shift) {
  if (layout != NEXSOS1_KBD_MAC19ITA) return NULL;
  if (code == 40U) return shift ? "\xC3\x80" : "\xC3\xA0";
  if (code == 26U) return shift ? "\xC3\xA9" : "\xC3\xA8";
  if (code == 39U && !shift) return "\xC3\xB2";
  if (code == 41U && !shift) return "\xC3\xB9";
  if (code == 43U && !shift) return "\xC3\xAC";
  return NULL;
}

static const char plain[128] = {
  0,0,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
  'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s',
  'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
  'b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0
};
static const char shifted_map[128] = {
  0,0,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
  'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S',
  'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V',
  'B','N','M','<','>','?',0,'*',0,' ',0,0,0,0,0,0
};

static int letter(unsigned c) {
  return (c >= 16 && c <= 25) || (c >= 30 && c <= 38) ||
         (c >= 44 && c <= 50);
}


static int keyboard_modifier(struct os1_keyboard_state *state,
                              unsigned code, int value) {
  if (code == 42U || code == 54U) {
    unsigned bit = code == 42U ? 1U : 2U;
    if (value) state->shift_keys |= bit;
    else state->shift_keys &= ~bit;
    return 1;
  }
  if (code == 29U || code == 97U) {
    unsigned bit = code == 29U ? 1U : 2U;
    if (value) state->ctrl_keys |= bit;
    else state->ctrl_keys &= ~bit;
    return 1;
  }
  if (code == 58U) {
    if (value == 1) state->caps = !state->caps;
    return 1;
  }
  return 0;
}

int os1_keyboard_translate(struct os1_keyboard_state *state, unsigned code,
                            int value, int layout,
                            struct os1_keyboard_key *out) {
  if (state == NULL || out == NULL || value < 0 || value > 2) return -1;
  if (keyboard_modifier(state, code, value)) return 0;
  int upper = state->shift_keys != 0U;
  if (letter(code)) upper ^= state->caps;
  unsigned char ch = code < sizeof(plain) ?
      (unsigned char)(upper ? shifted_map[code] : plain[code]) : 0U;
  int ctrl_key = 0;
  if (state->ctrl_keys && ch >= 'a' && ch <= 'z') {
    ch = (unsigned char)(ch - 'a' + 1);
    ctrl_key = 1;
  } else if (state->ctrl_keys && ch >= 'A' && ch <= 'Z') {
    ch = (unsigned char)(ch - 'A' + 1);
    ctrl_key = 1;
  }
  *out = (struct os1_keyboard_key){.code = code, .state = value, .key = ch};
  out->utf8[0] = (char)ch;
  if (value && !ctrl_key) {
    const char *utf8 = os1_keyboard_layout_utf8(layout, code, upper);
    if (utf8 != NULL) {
      out->utf8[0] = utf8[0];
      out->utf8[1] = utf8[1];
    }
  }
  return 1;
}
