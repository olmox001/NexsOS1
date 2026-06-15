#ifndef _DRIVERS_USB_USB_H
#define _DRIVERS_USB_USB_H

/*
 * kernel/include/drivers/usb/usb.h
 * USB core — controller-agnostic bus model.
 *
 * Layering (ASTRA): a host controller driver (xHCI / EHCI / UHCI) registers a
 * struct usb_hcd implementing usb_hcd_ops. The core enumerates each port,
 * reads standard descriptors over the control pipe, and hands HID interfaces
 * to usb_hid.c, which translates reports into the existing evdev input core
 * (virtio_input_add_event). The HCD never knows about HID; the HID driver
 * never knows whether the transport is xHCI/EHCI/UHCI.
 *
 * Addressing is delegated to the HCD via enumerate_port(): UHCI/EHCI send a
 * SET_ADDRESS on the default pipe, xHCI issues an Address Device command — the
 * core must not assume either.
 */

#include <kernel/types.h>

/* --- USB speeds --- */
#define USB_SPEED_LOW   1
#define USB_SPEED_FULL  2
#define USB_SPEED_HIGH  3
#define USB_SPEED_SUPER 4

/* --- Standard request types (bmRequestType) --- */
#define USB_DIR_OUT 0x00
#define USB_DIR_IN  0x80
#define USB_TYPE_STANDARD 0x00
#define USB_TYPE_CLASS    0x20
#define USB_RECIP_DEVICE    0x00
#define USB_RECIP_INTERFACE 0x01

/* --- Standard requests (bRequest) --- */
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_ADDRESS    0x05
#define USB_REQ_SET_CONFIG     0x09

/* HID class requests */
#define HID_REQ_SET_PROTOCOL 0x0B
#define HID_REQ_SET_IDLE     0x0A
#define HID_PROTO_BOOT 0
#define HID_PROTO_REPORT 1

/* --- Descriptor types --- */
#define USB_DT_DEVICE    0x01
#define USB_DT_CONFIG    0x02
#define USB_DT_INTERFACE 0x04
#define USB_DT_ENDPOINT  0x05
#define USB_DT_HID       0x21
#define USB_DT_HID_REPORT 0x22

/* --- Class codes --- */
#define USB_CLASS_HID 0x03
#define USB_CLASS_HUB 0x09
#define HID_SUBCLASS_BOOT 0x01
#define HID_PROTOCOL_KEYBOARD 0x01
#define HID_PROTOCOL_MOUSE    0x02

/* --- Hub class (USB 2.0 ch.11) --- */
#define USB_DT_HUB 0x29
#define USB_REQ_GET_STATUS    0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_FEATURE   0x03
#define HUB_FEAT_PORT_RESET   4
#define HUB_FEAT_PORT_POWER   8
#define HUB_FEAT_C_PORT_CONNECTION 16
#define HUB_FEAT_C_PORT_RESET 20
#define HUB_PORT_CONNECTION 0x0001
#define HUB_PORT_ENABLE     0x0002
#define HUB_PORT_RESET      0x0010
#define HUB_PORT_LOWSPEED   0x0200
#define HUB_PORT_HIGHSPEED  0x0400
#define USB_RECIP_OTHER 0x03

/* 8-byte SETUP packet */
struct usb_setup {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

struct usb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

struct usb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

struct usb_hcd;

/* A USB device the core enumerated and addressed. */
struct usb_device {
    struct usb_hcd *hcd;
    uint8_t  addr;            /* USB address (1..127), HCD-assigned */
    uint8_t  speed;
    int      port;            /* root-hub port index (for root devices) */
    uint8_t  max_packet0;     /* EP0 max packet */
    uint16_t vendor, product;
    uint8_t  dev_class, dev_subclass, dev_protocol;
    void    *hcd_priv;        /* HCD per-device state (xHCI slot, etc.) */

    /* Topology (for devices behind external hubs). */
    struct usb_device *parent; /* upstream hub, NULL if on the root hub */
    uint8_t  hub_port;         /* 1-based port on the parent hub */
    uint8_t  root_port;        /* 1-based root-hub port at the top of the path */
    uint32_t route;            /* xHCI route string (0 for root devices) */
    uint8_t  tier;             /* hub depth from root (0 = root device) */
    int      is_hub;

    /* HID interface of interest (filled by usb_hid bind) */
    int      hid_iface;
    uint8_t  hid_ep_in;       /* endpoint address (with 0x80 IN bit) */
    int      hid_ep_max;      /* wMaxPacketSize of that endpoint */
    uint8_t  hid_interval;
    uint8_t  hid_protocol;    /* HID_PROTOCOL_KEYBOARD / _MOUSE (boot) */
    int      hid_use_report;  /* 1 = report-protocol HID (non-boot, e.g. tablet) */
    /* Absolute pointer, parsed from the HID report descriptor (USB tablet). */
    int      hid_is_abs;      /* 1 = reports absolute X/Y */
    uint16_t hid_abs_x_off;   /* bit offset of X within the input report */
    uint16_t hid_abs_y_off;   /* bit offset of Y */
    uint8_t  hid_abs_bits;    /* X/Y field size in bits */
    uint32_t hid_abs_max;     /* logical max of X/Y (for scaling) */
    int16_t  hid_btn_off;     /* bit offset of the first button, -1 if none */
    int      in_use;
};

/* Host controller driver contract. */
struct usb_hcd_ops {
    int  (*num_ports)(struct usb_hcd *hcd);
    int  (*port_connected)(struct usb_hcd *hcd, int port);
    /* Reset + address a device on root-hub `port`; fill *out (addr/speed/
     * max_packet0/hcd_priv). Returns 0 on success, <0 on failure/no device. */
    int  (*enumerate_port)(struct usb_hcd *hcd, int port, struct usb_device *out);
    /* Address a device whose topology (parent/route/tier/root_port/speed) the
     * core already filled, reached through an external hub. The hub did the
     * port reset; this only assigns the address/slot. Optional (NULL = no hub
     * support on this controller). Returns 0 on success. */
    int  (*address_dev)(struct usb_hcd *hcd, struct usb_device *dev);
    /* Control transfer on the device's default pipe. data may be NULL.
     * Returns bytes transferred (>=0) or <0 on error. */
    int  (*control)(struct usb_hcd *hcd, struct usb_device *dev,
                    const struct usb_setup *setup, void *data, int len);
    /* Configure an interrupt IN endpoint for periodic polling. */
    int  (*intr_open)(struct usb_hcd *hcd, struct usb_device *dev,
                      uint8_t ep_addr, int max_packet, int interval);
    /* Non-blocking poll of the interrupt IN endpoint: copies up to len bytes
     * into buf. Returns bytes (0 if nothing new) or <0. */
    int  (*intr_poll)(struct usb_hcd *hcd, struct usb_device *dev,
                      uint8_t ep_addr, void *buf, int len);
};

struct usb_hcd {
    const char *name;
    struct usb_hcd_ops *ops;
    void *priv;
};

/* --- USB core API --- */

/* Called by each HCD after it has reset its controller. The core immediately
 * enumerates the controller's ports and binds HID devices. */
void usb_register_hcd(struct usb_hcd *hcd);

/* Control-transfer convenience used by the core and class drivers. */
int  usb_control(struct usb_device *dev, uint8_t bmRequestType, uint8_t bRequest,
                 uint16_t wValue, uint16_t wIndex, void *data, uint16_t wLength);

/* Subsystem entry point: register the HCD drivers and run driver matching so
 * each present controller is probed. Called from the input init path. */
void usb_init(void);

/* Periodic poll of all bound HID endpoints; drives the evdev input core.
 * Called from the same place keyboard_poll() is serviced. */
void usb_hid_poll(void);

/* Internal: usb_hid.c entry, called by the core for each enumerated device. */
void usb_hid_probe_device(struct usb_device *dev);

/* HCD driver registration hooks (each calls driver_register on its driver). */
void xhci_register(void);
void ehci_register(void);
void uhci_register(void);

#endif /* _DRIVERS_USB_USB_H */
