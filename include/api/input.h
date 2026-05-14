#ifndef _INPUT_H
#define _INPUT_H

#include <os1.h>
#include <posix_types.h>

/* Input event types */
#define INPUT_TYPE_KEYBOARD 1
#define INPUT_TYPE_MOUSE    2

/* Key states */
#define KEY_RELEASED 0
#define KEY_PRESSED  1
#define KEY_REPEAT   2

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
    };
} input_event_t;

/**
 * Poll for an input event.
 * Returns 1 if an event was retrieved, 0 if no events are pending, -1 on error.
 */
int input_poll_event(input_event_t *event);

#endif
