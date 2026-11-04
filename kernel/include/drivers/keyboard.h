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

/* Input providers are tagged for bounded post-mortem tracing. */
enum input_provider {
  INPUT_PROVIDER_UNKNOWN = 0,
  INPUT_PROVIDER_VIRTIO = 1,
  INPUT_PROVIDER_PS2 = 2,
  INPUT_PROVIDER_USB_HID = 3,
  INPUT_PROVIDER_LEGACY = 4
};

/* Unified input sink. Every input provider reports raw evdev events here.
 * The current kernel consumer still performs layout/character semantics and
 * focused-PID routing; the planned nxinput service will own those semantics. */
void input_report_from(uint8_t provider, uint16_t type, uint16_t code,
                       int32_t value);
void input_report(uint16_t type, uint16_t code, int32_t value);
/* Fault-safe dump of the bounded input event history and drop counters. */
void input_trace_dump(void);
/* input_server_start - launch the input server kernel thread (task-context
 * dispatch, IRQ-decoupled).  Call after the scheduler + compositor are up. */
void input_server_start(void);
/* input_drain - dispatch queued raw input events in process context from
 * SYS_COMPOSITOR_RENDER. Hardware input IRQs only enqueue events. */
void input_drain(void);

#include <kernel/sched.h>
extern struct wait_queue_head keyboard_wait_queue;

#endif /* _DRIVERS_KEYBOARD_H */
