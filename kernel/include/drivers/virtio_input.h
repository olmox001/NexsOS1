/*
 * kernel/include/drivers/virtio_input.h
 * VirtIO Input Device Driver
 */
#ifndef _DRIVERS_VIRTIO_INPUT_H
#define _DRIVERS_VIRTIO_INPUT_H

#include <kernel/types.h>

/* VirtIO Input Event Types (Linux input.h compatible) */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04

/* Relative Axes */
#define REL_X 0x00
#define REL_Y 0x01
#define REL_WHEEL 0x08

/* Absolute Axes */
#define ABS_X 0x00
#define ABS_Y 0x01

/* Input contract for absolute pointers: EV_ABS values are normalized to
 * [0, INPUT_ABS_MAX]. A provider scales its device's logical range to this; the
 * consumer (the compositor) scales [0, INPUT_ABS_MAX] to framebuffer pixels.
 * QEMU's virtio-input/tablet already reports absolute axes in this range, so the
 * virtio-input driver forwards them unchanged. */
#define INPUT_ABS_MAX 0x7FFF

/* Mouse Buttons */
#define BTN_MOUSE 0x110
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112

/* Common Key Codes */
#define KEY_RESERVED 0
#define KEY_ESC 1
#define KEY_1 2
#define KEY_2 3
#define KEY_3 4
#define KEY_4 5
#define KEY_5 6
#define KEY_6 7
#define KEY_7 8
#define KEY_8 9
#define KEY_9 10
#define KEY_0 11
#define KEY_MINUS 12
#define KEY_EQUAL 13
#define KEY_BACKSPACE 14
#define KEY_TAB 15
#define KEY_Q 16
#define KEY_W 17
#define KEY_E 18
#define KEY_R 19
#define KEY_T 20
#define KEY_Y 21
#define KEY_U 22
#define KEY_I 23
#define KEY_O 24
#define KEY_P 25
#define KEY_LEFTBRACE 26
#define KEY_RIGHTBRACE 27
#define KEY_ENTER 28
#define KEY_LEFTCTRL 29
#define KEY_A 30
#define KEY_S 31
#define KEY_D 32
#define KEY_F 33
#define KEY_G 34
#define KEY_H 35
#define KEY_J 36
#define KEY_K 37
#define KEY_L 38
#define KEY_SEMICOLON 39
#define KEY_APOSTROPHE 40
#define KEY_GRAVE 41
#define KEY_LEFTSHIFT 42
#define KEY_BACKSLASH 43
#define KEY_Z 44
#define KEY_X 45
#define KEY_C 46
#define KEY_V 47
#define KEY_B 48
#define KEY_N 49
#define KEY_M 50
#define KEY_COMMA 51
#define KEY_DOT 52
#define KEY_SLASH 53
#define KEY_RIGHTSHIFT 54
#define KEY_KPASTERISK 55
#define KEY_LEFTALT 56
#define KEY_SPACE 57
#define KEY_CAPSLOCK 58
#define KEY_F1 59
#define KEY_F2 60
#define KEY_F3 61
#define KEY_F4 62
#define KEY_F5 63
#define KEY_F6 64
#define KEY_F7 65
#define KEY_F8 66
#define KEY_F9 67
#define KEY_F10 68
#define KEY_UP 103
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_DOWN 108

/* Input Event Structure (8 bytes) */
struct virtio_input_event {
  uint16_t type;
  uint16_t code;
  int32_t value;
} __attribute__((packed));

/* Driver API */
void virtio_input_init(void);
int virtio_input_poll(struct virtio_input_event *event);
int virtio_input_has_event(void);
void virtio_input_add_event(uint16_t type, uint16_t code, int32_t value);
int virtio_input_get_dev_count(void);

#endif /* _DRIVERS_VIRTIO_INPUT_H */
