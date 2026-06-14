/*
 * kernel/drivers/usb/usb_core.c
 * USB core: port enumeration, standard descriptor reads, HID hand-off.
 *
 * Controller-agnostic. Each HCD (xHCI/EHCI/UHCI) registers via
 * usb_register_hcd() after resetting its controller; the core then walks the
 * root-hub ports, addresses each device through the HCD (enumerate_port),
 * reads its descriptors over the control pipe, and registers any HID boot
 * interface for periodic polling (usb_hid.c).
 */

#include <drivers/usb/usb.h>
#include <kernel/driver.h>
#include <kernel/printk.h>
#include <kernel/string.h>

#define MAX_USB_DEVICES 16

static struct usb_device usb_devices[MAX_USB_DEVICES];

static struct usb_device *usb_alloc_device(void) {
    for (int i = 0; i < MAX_USB_DEVICES; i++) {
        if (!usb_devices[i].in_use) {
            memset(&usb_devices[i], 0, sizeof(usb_devices[i]));
            usb_devices[i].in_use = 1;
            usb_devices[i].hid_iface = -1;
            return &usb_devices[i];
        }
    }
    return NULL;
}

static void usb_setup_device(struct usb_device *dev);
static void usb_hub_init(struct usb_device *hub);

int usb_control(struct usb_device *dev, uint8_t bmRequestType, uint8_t bRequest,
                uint16_t wValue, uint16_t wIndex, void *data, uint16_t wLength) {
    struct usb_setup s = {
        .bmRequestType = bmRequestType,
        .bRequest = bRequest,
        .wValue = wValue,
        .wIndex = wIndex,
        .wLength = wLength,
    };
    if (!dev->hcd || !dev->hcd->ops->control) return -1;
    return dev->hcd->ops->control(dev->hcd, dev, &s, data, wLength);
}

/*
 * Parse the configuration descriptor block, locate a HID boot keyboard/mouse
 * interface and its interrupt-IN endpoint, and record them on the device.
 * Returns 1 if a usable HID interface was found.
 */
static int usb_parse_hid(struct usb_device *dev, const uint8_t *cfg, int len) {
    int i = 0;
    int cur_iface = -1;
    int cur_is_hid = 0;
    uint8_t cur_proto = 0;
    while (i + 2 <= len) {
        uint8_t blen = cfg[i];
        uint8_t btype = cfg[i + 1];
        if (blen < 2 || i + blen > len) break;

        if (btype == USB_DT_INTERFACE && blen >= sizeof(struct usb_interface_descriptor)) {
            const struct usb_interface_descriptor *id =
                (const struct usb_interface_descriptor *)(cfg + i);
            cur_iface = id->bInterfaceNumber;
            cur_is_hid = (id->bInterfaceClass == USB_CLASS_HID &&
                          id->bInterfaceSubClass == HID_SUBCLASS_BOOT &&
                          (id->bInterfaceProtocol == HID_PROTOCOL_KEYBOARD ||
                           id->bInterfaceProtocol == HID_PROTOCOL_MOUSE));
            cur_proto = id->bInterfaceProtocol;
        } else if (btype == USB_DT_ENDPOINT && cur_is_hid && dev->hid_iface < 0 &&
                   blen >= sizeof(struct usb_endpoint_descriptor)) {
            const struct usb_endpoint_descriptor *ed =
                (const struct usb_endpoint_descriptor *)(cfg + i);
            int is_in = (ed->bEndpointAddress & 0x80) != 0;
            int is_intr = (ed->bmAttributes & 0x03) == 0x03;
            if (is_in && is_intr) {
                dev->hid_iface = cur_iface;
                dev->hid_ep_in = ed->bEndpointAddress;
                dev->hid_ep_max = ed->wMaxPacketSize & 0x7FF;
                dev->hid_interval = ed->bInterval;
                dev->hid_protocol = cur_proto;
                return 1;
            }
        }
        i += blen;
    }
    return 0;
}

static void usb_setup_device(struct usb_device *dev) {
    struct usb_device_descriptor dd;
    memset(&dd, 0, sizeof(dd));

    /* Full device descriptor. */
    int r = usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                        USB_REQ_GET_DESCRIPTOR, (USB_DT_DEVICE << 8), 0,
                        &dd, sizeof(dd));
    if (r < (int)sizeof(dd)) {
        pr_warn("USB: device descriptor read failed on port %d (r=%d)\n", dev->port, r);
        return;
    }
    dev->vendor = dd.idVendor;
    dev->product = dd.idProduct;
    dev->dev_class = dd.bDeviceClass;
    dev->dev_subclass = dd.bDeviceSubClass;
    dev->dev_protocol = dd.bDeviceProtocol;
    if (dd.bMaxPacketSize0) dev->max_packet0 = dd.bMaxPacketSize0;

    pr_info("USB: device %04x:%04x class %02x on port %d (addr %d, speed %d)\n",
            dev->vendor, dev->product, dev->dev_class, dev->port, dev->addr, dev->speed);

    /* Configuration descriptor: first 9 bytes for wTotalLength, then the whole
     * block (interfaces + endpoints). */
    uint8_t cfg[256];
    memset(cfg, 0, sizeof(cfg));
    r = usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, (USB_DT_CONFIG << 8), 0, cfg, 9);
    if (r < 9) {
        pr_warn("USB: config descriptor header read failed (r=%d)\n", r);
        return;
    }
    const struct usb_config_descriptor *cd = (const struct usb_config_descriptor *)cfg;
    int total = cd->wTotalLength;
    if (total > (int)sizeof(cfg)) total = sizeof(cfg);
    if (total > 9) {
        r = usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                        USB_REQ_GET_DESCRIPTOR, (USB_DT_CONFIG << 8), 0, cfg, total);
        if (r < total) total = (r > 0) ? r : 9;
    }
    uint8_t cfg_value = cd->bConfigurationValue;

    /* Select the configuration. */
    usb_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                USB_REQ_SET_CONFIG, cfg_value, 0, NULL, 0);

    /* A hub: bring up its downstream ports and recurse. */
    if (dev->dev_class == USB_CLASS_HUB) {
        dev->is_hub = 1;
        usb_hub_init(dev);
        return;
    }

    if (!usb_parse_hid(dev, cfg, total)) {
        return; /* not a HID boot device; ignored for now */
    }

    /* Put the interface into boot protocol and zero idle so reports only arrive
     * on change, then open the interrupt endpoint and register for polling. */
    usb_control(dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                HID_REQ_SET_PROTOCOL, HID_PROTO_BOOT, dev->hid_iface, NULL, 0);
    usb_control(dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                HID_REQ_SET_IDLE, 0, dev->hid_iface, NULL, 0);

    if (dev->hcd->ops->intr_open)
        dev->hcd->ops->intr_open(dev->hcd, dev, dev->hid_ep_in,
                                 dev->hid_ep_max, dev->hid_interval);

    pr_info("USB: HID %s on iface %d ep 0x%02x (max %d)\n",
            dev->hid_protocol == HID_PROTOCOL_KEYBOARD ? "keyboard" : "mouse",
            dev->hid_iface, dev->hid_ep_in, dev->hid_ep_max);

    usb_hid_probe_device(dev);
}

/* Hub class request helpers (USB 2.0 ch.11). */
static int hub_port_feature(struct usb_device *hub, uint8_t req, uint16_t feat, uint16_t port) {
    return usb_control(hub, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
                       req, feat, port, NULL, 0);
}
static uint16_t hub_port_status(struct usb_device *hub, uint16_t port) {
    uint8_t st[4];
    memset(st, 0, sizeof(st));
    if (usb_control(hub, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
                    USB_REQ_GET_STATUS, 0, port, st, 4) < 2)
        return 0;
    return (uint16_t)(st[0] | (st[1] << 8));
}

/*
 * usb_hub_init - enumerate an external hub's downstream ports.
 *
 * Controller-agnostic: powers and resets each port via hub class requests, then
 * asks the HCD to address the device through ops->address_dev (the HCD builds
 * the xHCI route string / assigns the UHCI address). Recurses for nested hubs.
 */
static void usb_hub_init(struct usb_device *hub) {
    if (hub->tier >= 5) return; /* USB max topology depth */
    if (!hub->hcd->ops->address_dev) return; /* controller without hub support */

    uint8_t hd[16];
    memset(hd, 0, sizeof(hd));
    int r = usb_control(hub, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                        USB_REQ_GET_DESCRIPTOR, (USB_DT_HUB << 8), 0, hd, 9);
    if (r < 3) return;
    int nports = hd[2];
    pr_info("USB: hub on root port %d: %d downstream ports (tier %d)\n",
            hub->root_port, nports, hub->tier);

    for (int p = 1; p <= nports; p++)
        hub_port_feature(hub, USB_REQ_SET_FEATURE, HUB_FEAT_PORT_POWER, p);
    for (volatile long s = 0; s < 4000000L; s++) { /* power-on settle */ }

    for (int p = 1; p <= nports; p++) {
        uint16_t status = hub_port_status(hub, p);
        if (!(status & HUB_PORT_CONNECTION)) continue;
        hub_port_feature(hub, USB_REQ_CLEAR_FEATURE, HUB_FEAT_C_PORT_CONNECTION, p);

        hub_port_feature(hub, USB_REQ_SET_FEATURE, HUB_FEAT_PORT_RESET, p);
        for (int t = 0; t < 20; t++) {
            for (volatile long s = 0; s < 1000000L; s++) { }
            status = hub_port_status(hub, p);
            if (status & HUB_PORT_ENABLE) break;
        }
        hub_port_feature(hub, USB_REQ_CLEAR_FEATURE, HUB_FEAT_C_PORT_RESET, p);
        if (!(status & HUB_PORT_ENABLE)) continue;

        int speed = (status & HUB_PORT_LOWSPEED)  ? USB_SPEED_LOW
                  : (status & HUB_PORT_HIGHSPEED) ? USB_SPEED_HIGH
                                                  : USB_SPEED_FULL;

        struct usb_device *child = usb_alloc_device();
        if (!child) return;
        child->hcd = hub->hcd;
        child->parent = hub;
        child->hub_port = p;
        child->root_port = hub->root_port;
        child->tier = hub->tier + 1;
        child->route = hub->route | ((uint32_t)(p & 0xF) << (4 * hub->tier));
        child->speed = speed;
        child->max_packet0 = (speed == USB_SPEED_LOW) ? 8
                           : (speed == USB_SPEED_SUPER) ? 512 : 64;

        if (hub->hcd->ops->address_dev(hub->hcd, child) < 0) {
            child->in_use = 0;
            continue;
        }
        usb_setup_device(child);
    }
}

void usb_register_hcd(struct usb_hcd *hcd) {
    if (!hcd || !hcd->ops) return;
    int nports = hcd->ops->num_ports ? hcd->ops->num_ports(hcd) : 0;
    pr_info("USB: %s registered, %d root ports\n", hcd->name, nports);

    for (int port = 0; port < nports; port++) {
        if (hcd->ops->port_connected && !hcd->ops->port_connected(hcd, port))
            continue;
        struct usb_device *dev = usb_alloc_device();
        if (!dev) {
            pr_warn("%s", "USB: device table full\n");
            return;
        }
        dev->hcd = hcd;
        dev->port = port;
        dev->root_port = port + 1; /* 1-based for the xHCI slot context */
        dev->route = 0;            /* root device: empty route string */
        dev->tier = 0;
        dev->max_packet0 = 8; /* default until descriptor read */
        if (hcd->ops->enumerate_port(hcd, port, dev) < 0) {
            dev->in_use = 0;
            continue;
        }
        usb_setup_device(dev);
    }
}

void usb_init(void) {
    pr_info("%s", "USB: Initializing host controllers...\n");
    xhci_register();
    ehci_register();
    uhci_register();
    driver_match_all();
}
