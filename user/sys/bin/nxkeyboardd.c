/* nxkeyboardd: ring-3 keyboard layout and focused-client event router. */
#include <os1.h>
#include <os1_input.h>
#include <stdint.h>

#define EV_KEY 1
#define KEY_LEFTCTRL 29
#define KEY_LEFTSHIFT 42
#define KEY_RIGHTSHIFT 54
#define KEY_CAPSLOCK 58
#define KEY_RIGHTCTRL 97

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

struct keyboard_state { int shift, ctrl, caps; };

static void route_key(unsigned code, int value, struct keyboard_state *state) {
  if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) { state->shift = value != 0; return; }
  if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) { state->ctrl = value != 0; return; }
  if (code == KEY_CAPSLOCK && value == 1) { state->caps = !state->caps; return; }
  int upper = state->shift;
  unsigned char ch = 0;
  if (letter(code)) upper ^= state->caps;
  if (code < 128)
    ch = (unsigned char)(upper ? shifted_map[code] : plain[code]);
  int ctrl_key = 0;
  if (state->ctrl && ch >= 'a' && ch <= 'z') { ch = (unsigned char)(ch - 'a' + 1); ctrl_key = 1; }
  else if (state->ctrl && ch >= 'A' && ch <= 'Z') { ch = (unsigned char)(ch - 'A' + 1); ctrl_key = 1; }
  if (value && !ctrl_key) {
    if (code == 40) ch = upper ? 0xc0 : 0xe0;
    else if (code == 26) ch = upper ? 0xe9 : 0xe8;
    else if (code == 39) ch = 0xf2;
    else if (code == 41) ch = 0xf9;
    else if (code == 43) ch = 0xec;
  }
  struct window_info windows[40];
  long n = OS1_window_enum(windows, 40);
  int focus = 0;
  for (long i = 0; i < n && i < 40; ++i)
    if (windows[i].flags & WININFO_FOCUSED) { focus = windows[i].pid; break; }
  if (focus <= 0) return;
  struct ipc_message m = {0};
  m.type = IPC_TYPE_INPUT;
  m.data1 = ((uint64_t)code << 16) | ch;
  m.data2 = (uint64_t)value;
  m.payload[0] = (char)ch;
  m.payload[1] = 0;
  if (value && !ctrl_key) {
    const char *utf8 = NULL;
    if (code == 40) utf8 = upper ? "\xC3\x80" : "\xC3\xA0";
    else if (code == 26) utf8 = upper ? "\xC3\x89" : "\xC3\xA8";
    else if (code == 39) utf8 = "\xC3\xB2";
    else if (code == 41) utf8 = "\xC3\xB9";
    else if (code == 43) utf8 = "\xC3\xAC";
    if (utf8) { m.payload[0] = utf8[0]; m.payload[1] = utf8[1]; m.payload[2] = 0; }
  }
  (void)OS1low_ipc_send(focus, &m);
}

int main(void) {
  static struct keyboard_state state[256];
  print("[nxkeyboardd] ring-3 keyboard service started\n");
  for (;;) {
    struct os1_raw_input_event_v1 event = {
        .abi_version = OS1_INPUT_ABI_V1, .struct_size = sizeof(event)};
    long result = OS1_input_wait_keyboard(&event);

    if (result > 0) {
      if (event.type == EV_KEY)
        route_key(event.code, event.value, &state[event.device_id]);
      continue;
    }
    if (result == -EAGAIN) {
      OS1_sleep(5);
      continue;
    }
    printf("[nxkeyboardd] raw input wait failed: %ld\n", result);
    OS1_sleep(50);
  }
}
