/*
 * kernel/drivers/usb/usb_hid.c
 * USB HID boot-protocol class driver.
 *
 * Translates boot keyboard (8-byte) and boot mouse (3-4 byte) reports into the
 * existing evdev input core (virtio_input_add_event), exactly as the PS/2 and
 * virtio-input drivers do. usb_hid_poll() is called from the same place that
 * services keyboard_poll().
 */

#include <drivers/usb/usb.h>
#include <drivers/virtio_input.h>
#include <drivers/keyboard.h>
#include <kernel/printk.h>
#include <kernel/string.h>

/* HID reports are translated to evdev events and pushed through the unified
 * input_report() sink (keyboard.c), exactly like virtio-input and PS/2. */

#define MAX_HID_DEVICES 8

struct hid_dev {
    struct usb_device *dev;
    uint8_t prev[8];   /* last report (for press/release diffing) */
    int     in_use;
};

static struct hid_dev hid_devs[MAX_HID_DEVICES];

/*
 * HID usage -> evdev keycode (Linux drivers/hid/usbhid/usbkbd.c hid_keyboard[]).
 * Output keycodes match the KEY_* numbering in virtio_input.h (evdev), so the
 * existing keyboard.c layout logic handles them unchanged.
 */
static const uint8_t hid_to_evdev[256] = {
    0, 0, 0, 0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
    50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, 2, 3,
    4, 5, 6, 7, 8, 9, 10, 11, 28, 1, 14, 15, 57, 12, 13, 26,
    27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
    65, 66, 67, 68, 87, 88, 99, 70, 119, 110, 102, 104, 111, 107, 109, 106,
    105, 108, 103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
    72, 73, 82, 83, 86, 127, 116, 117, 183, 184, 185, 186, 187, 188, 189, 190,
    191, 192, 193, 194, 134, 138, 130, 132, 128, 129, 131, 137, 133, 135, 136, 113,
    115, 114, 0, 0, 0, 121, 0, 89, 93, 124, 92, 94, 95, 0, 0, 0,
    122, 123, 90, 91, 85, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    29, 42, 56, 125, 97, 54, 100, 126, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/* Modifier byte bit -> evdev keycode (LCtrl..RGui). */
static const uint8_t mod_to_evdev[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };

void usb_hid_probe_device(struct usb_device *dev) {
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        if (!hid_devs[i].in_use) {
            hid_devs[i].in_use = 1;
            hid_devs[i].dev = dev;
            memset(hid_devs[i].prev, 0, sizeof(hid_devs[i].prev));
            return;
        }
    }
    pr_warn("%s", "USB-HID: device table full\n");
}

static void hid_handle_keyboard(struct hid_dev *h, const uint8_t *r, int len) {
    if (len < 8) return;
    const uint8_t *prev = h->prev;

    /* Modifiers: press/release on bit change. */
    uint8_t changed = r[0] ^ prev[0];
    for (int b = 0; b < 8; b++) {
        if (changed & (1 << b))
            input_report(EV_KEY, mod_to_evdev[b], (r[0] >> b) & 1);
    }

    /* Released keys: present in prev[2..7], absent in new. */
    for (int i = 2; i < 8; i++) {
        uint8_t k = prev[i];
        if (k <= 3) continue;
        int still = 0;
        for (int j = 2; j < 8; j++) if (r[j] == k) { still = 1; break; }
        if (!still) input_report(EV_KEY, hid_to_evdev[k], 0);
    }
    /* Pressed keys: present in new, absent in prev. */
    for (int i = 2; i < 8; i++) {
        uint8_t k = r[i];
        if (k <= 3) continue;
        int was = 0;
        for (int j = 2; j < 8; j++) if (prev[j] == k) { was = 1; break; }
        if (!was) input_report(EV_KEY, hid_to_evdev[k], 1);
    }
    memcpy(h->prev, r, 8);
    input_report(EV_SYN, 0, 0);
}

static void hid_handle_mouse(struct hid_dev *h, const uint8_t *r, int len) {
    if (len < 3) return;
    uint8_t buttons = r[0];
    int8_t dx = (int8_t)r[1];
    int8_t dy = (int8_t)r[2];   /* HID Y grows downward; compositor uses the same */

    /* Buttons: report on change; motion as relative deltas; EV_SYN repaints. */
    uint8_t changed = buttons ^ h->prev[0];
    if (changed & 0x01) input_report(EV_KEY, BTN_LEFT, buttons & 0x01);
    if (changed & 0x02) input_report(EV_KEY, BTN_RIGHT, (buttons >> 1) & 1);
    if (changed & 0x04) input_report(EV_KEY, BTN_MIDDLE, (buttons >> 2) & 1);

    if (dx) input_report(EV_REL, REL_X, dx);
    if (dy) input_report(EV_REL, REL_Y, dy);

    h->prev[0] = buttons;
    input_report(EV_SYN, 0, 0);
}

void usb_hid_poll(void) {
    uint8_t report[64];
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        struct hid_dev *h = &hid_devs[i];
        if (!h->in_use) continue;
        struct usb_device *dev = h->dev;
        if (!dev->hcd->ops->intr_poll) continue;

        int n = dev->hcd->ops->intr_poll(dev->hcd, dev, dev->hid_ep_in,
                                         report, sizeof(report));
        while (n > 0) {
            if (dev->hid_protocol == HID_PROTOCOL_KEYBOARD)
                hid_handle_keyboard(h, report, n);
            else
                hid_handle_mouse(h, report, n);
            n = dev->hcd->ops->intr_poll(dev->hcd, dev, dev->hid_ep_in,
                                         report, sizeof(report));
        }
    }
}
