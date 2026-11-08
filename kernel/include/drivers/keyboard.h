/*
 * kernel/include/drivers/keyboard.h
 * Keyboard Input Subsystem
 */
#ifndef _DRIVERS_KEYBOARD_H
#define _DRIVERS_KEYBOARD_H

#include <kernel/types.h>

struct os1_raw_input_event_v1;

/* Initialize keyboard subsystem */
void keyboard_init(void);

/* Poll for new keyboard input */
void keyboard_poll(void);
long input_next_raw_event(struct os1_raw_input_event_v1 *user_event,
                          unsigned role);

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

enum input_device_id {
  INPUT_DEVICE_VIRTIO_BASE = 0x10,
  INPUT_DEVICE_PS2_KEYBOARD = 0x21,
  INPUT_DEVICE_PS2_MOUSE = 0x22,
  INPUT_DEVICE_USB_HID_BASE = 0x30
};

/* Unified raw input sink. Every input provider only enqueues events; the
 * supervised nxkeyboardd and nxmoused processes own all routing policy. */
void input_report_from(uint8_t provider, uint16_t type, uint16_t code,
                       int32_t value);
void input_report_from_device(uint8_t provider, uint8_t device_id,
                              uint16_t type, uint16_t code, int32_t value);
void input_report(uint16_t type, uint16_t code, int32_t value);
/* Fault-safe dump of the bounded input event history and drop counters. */
void input_trace_dump(void);
/* Retained compatibility no-op. Input services run in userland. */
void input_server_start(void);

#include <kernel/sched.h>
extern struct wait_queue_head keyboard_wait_queue;

#endif /* _DRIVERS_KEYBOARD_H */
