#ifndef _INPUT_H
#define _INPUT_H

#include <os1.h>
#include <posix_types.h>

/* Input event types */
#define INPUT_TYPE_KEYBOARD 1
#define INPUT_TYPE_MOUSE    2
#define INPUT_TYPE_RESIZE   3  /* window/desktop resized: event.resize.w/h (GFX-DYN-01) */

/* Key states */
#define KEY_RELEASED 0
#define KEY_PRESSED  1
#define KEY_REPEAT   2

/* Mouse buttons — evdev BTN_* codes as delivered in input_event_t.mouse.button
 * (the compositor forwards the raw evdev code, NOT a 0/1/2 index).  Shared here
 * so every app matches the same constants instead of redefining them. */
#ifndef MOUSE_BTN_LEFT
#define MOUSE_BTN_LEFT   0x110
#define MOUSE_BTN_RIGHT  0x111
#define MOUSE_BTN_MIDDLE 0x112
#endif

/* evdev scancodes for special (non-ASCII) keys, reported in
 * input_event_t.keyboard.scancode.  Windowed apps that need cursor/navigation
 * keys match on these (the ASCII .key byte is 0 for them).  Values mirror the
 * Linux/virtio-input keycodes the driver emits. */
#define INPUT_KEY_ESC        1
#define INPUT_KEY_BACKSPACE  14
#define INPUT_KEY_TAB        15
#define INPUT_KEY_ENTER      28
#define INPUT_KEY_UP         103
#define INPUT_KEY_LEFT       105
#define INPUT_KEY_RIGHT      106
#define INPUT_KEY_DOWN       108

typedef struct {
    int type;
    union {
        struct {
            unsigned char key;
            int state;
            uint16_t scancode;
            char utf8[8];
        } keyboard;
        struct {
            int button;
            int state;
            int x, y;
        } mouse;
        struct {
            int w, h; /* new logical size of the window (or desktop) */
        } resize;
    };
} input_event_t;

/**
 * Poll for an input event.
 * Returns 1 if an event was retrieved, 0 if no events are pending, -1 on error.
 */
int input_poll_event(input_event_t *event);

#endif
