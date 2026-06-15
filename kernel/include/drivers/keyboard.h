/*
 * kernel/include/drivers/keyboard.h
 * Keyboard Input Subsystem
 */
#ifndef _DRIVERS_KEYBOARD_H
#define _DRIVERS_KEYBOARD_H

#include <kernel/types.h>

/* Initialize keyboard subsystem */
void keyboard_init(void);

/* Poll for new keyboard input */
void keyboard_poll(void);

/* Check if input is available */
int keyboard_has_input(void);

/* Read one character (non-blocking, returns -1 if none) */
int keyboard_read_char_nonblock(void);

/* Read one character (blocking) */
char keyboard_read_char(void);

/* Read a line of input (blocking, with echo) */
/* Read a line of input (blocking, with echo) */
int keyboard_read_line(char *buf, int max_len);

/* Notification from low-level driver (VirtIO) */
void keyboard_notify_input(void);

/* Unified input sink. Every input provider — virtio-input, PS/2, USB HID —
 * reports evdev events (EV_KEY/EV_REL/EV_ABS/EV_SYN) here, and this one place
 * routes them: keys -> keyboard layout + IPC to the focused process, pointer
 * motion/buttons -> compositor. Providers must not dispatch on their own. */
void input_report(uint16_t type, uint16_t code, int32_t value);

#include <kernel/sched.h>
extern struct wait_queue_head keyboard_wait_queue;

#endif /* _DRIVERS_KEYBOARD_H */
