/*
 * kernel/drivers/usb/uhci.c
 * UHCI (USB 1.1) host controller driver — polled.
 *
 * UHCI is the companion controller that carries full/low-speed devices on
 * i440fx and on UTM "USB 2.0" (the EHCI routes 1.1 devices to it), so a USB
 * boot keyboard/mouse/tablet ends up here. Port-I/O based; schedule is a 1024-
 * entry frame list pointing at a skeleton of QHs (interrupt then control).
 * Control transfers run synchronously; interrupt IN endpoints are re-armed and
 * polled from usb_hid_poll() via the same timer tick as xHCI.
 */

#include <drivers/usb/usb.h>
#include <drivers/pci.h>
#include <kernel/driver.h>
#include <kernel/hal.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>

/* --- I/O registers (offset from BAR0 I/O base) --- */
#define UHCI_USBCMD    0x00
#define UHCI_USBSTS    0x02
#define UHCI_USBINTR   0x04
#define UHCI_FRNUM     0x06
#define UHCI_FRBASEADD 0x08
#define UHCI_SOFMOD    0x0C
#define UHCI_PORTSC1   0x10

#define CMD_RUN     0x0001
#define CMD_HCRESET 0x0002
#define CMD_GRESET  0x0004
#define CMD_MAXP    0x0080

#define PORT_CCS  0x0001
#define PORT_CSC  0x0002
#define PORT_PED  0x0004
#define PORT_PEDC 0x0008
#define PORT_LS   0x0100  /* low-speed device attached */
#define PORT_PR   0x0200  /* port reset */

/* Link pointer flags */
#define LP_TERMINATE 0x1
#define LP_QH        0x2
#define LP_VF        0x4  /* depth-first (TD only) */

/* TD ctrl_status */
#define TD_ACTIVE  (1u << 23)
#define TD_IOC     (1u << 24)
#define TD_LS      (1u << 26)
#define TD_CERR3   (3u << 27)
#define TD_SPD     (1u << 29)
#define TD_ACTLEN_MASK 0x7FF

/* PIDs */
#define PID_SETUP 0x2D
#define PID_IN    0x69
#define PID_OUT   0xE1

struct uhci_td {
    volatile uint32_t link;
    volatile uint32_t ctrl_status;
    volatile uint32_t token;
    volatile uint32_t buffer;
    uint32_t pad[4]; /* pad to 32B; alignment + software use */
} __attribute__((packed));

struct uhci_qh {
    volatile uint32_t head; /* next QH */
    volatile uint32_t elem; /* first TD */
    uint32_t pad[2];
} __attribute__((packed));

/* Per-interrupt-endpoint state. */
struct uhci_ep {
    struct uhci_qh *qh;      /* VA */
    uint64_t qh_phys;
    struct uhci_td *td;      /* VA */
    uint64_t td_phys;
    void *buf;               /* VA */
    uint64_t buf_phys;
    int toggle;
    int maxlen;
    int low_speed;
    uint8_t addr;
    uint8_t ep;
    int active;
};

struct uhci {
    uint16_t io;
    uint32_t *frame;         /* VA of frame list */
    uint64_t frame_phys;
    struct uhci_qh *qh_ctrl; /* VA */
    uint64_t qh_ctrl_phys;
    struct uhci_td *ctrl_td; /* VA: pool of control TDs */
    uint64_t ctrl_td_phys;
    void *ctrl_buf;          /* VA: control data bounce */
    uint64_t ctrl_buf_phys;

    struct uhci_ep eps[4];
    int ep_count;
    uint8_t next_addr;
    struct usb_hcd hcd;
};

static struct uhci uhci_ctrls[2];
static int uhci_count;

static inline void *page_va(uint64_t *phys_out) {
    if (phys_out) *phys_out = 0;
    void *p = pmm_alloc_page();
    if (!p) return NULL;
    memset(p, 0, PAGE_SIZE);
    if (phys_out) *phys_out = virt_to_phys(p);
    return p;
}

/* --- TD/control helpers --- */
static uint32_t mk_token(uint8_t pid, uint8_t addr, uint8_t ep, int toggle, int len) {
    uint32_t maxlen = (len == 0) ? 0x7FF : (uint32_t)(len - 1);
    return (uint32_t)pid | ((uint32_t)addr << 8) | ((uint32_t)ep << 15) |
           ((uint32_t)(toggle & 1) << 19) | (maxlen << 21);
}

/* Run a TD chain through the control QH and wait for completion.
 * Returns 0 on success, -1 on error/timeout. */
static int uhci_run_ctrl(struct uhci *u, uint64_t first_td_phys) {
    u->qh_ctrl->elem = (uint32_t)first_td_phys; /* not terminated, not QH */
    /* Wait until the QH element terminates (chain consumed) or a TD errors. */
    for (long s = 0; s < 50000000L; s++) {
        if (u->qh_ctrl->elem & LP_TERMINATE) break;
        /* error: scan control TD pool for a non-active stalled TD */
    }
    int rc = 0;
    if (!(u->qh_ctrl->elem & LP_TERMINATE)) rc = -1;
    u->qh_ctrl->elem = LP_TERMINATE;
    return rc;
}

static int uhci_control(struct usb_hcd *hcd, struct usb_device *dev,
                        const struct usb_setup *setup, void *data, int len) {
    struct uhci *u = hcd->priv;
    int ls = (dev->speed == USB_SPEED_LOW) ? TD_LS : 0;
    uint8_t addr = dev->addr;
    int mp = dev->max_packet0 ? dev->max_packet0 : 8;
    int dir_in = (setup->bmRequestType & USB_DIR_IN) != 0;

    struct uhci_td *td = u->ctrl_td;
    int n = 0;

    /* SETUP (DATA0). */
    memcpy(u->ctrl_buf, setup, 8);
    td[n].ctrl_status = TD_ACTIVE | TD_CERR3 | ls;
    td[n].token = mk_token(PID_SETUP, addr, 0, 0, 8);
    td[n].buffer = (uint32_t)u->ctrl_buf_phys;
    n++;

    /* DATA stage (toggle starts at 1), split into max-packet chunks. */
    int toggle = 1;
    int remaining = len;
    uint32_t off = 64; /* keep data after the 8-byte setup in the bounce page */
    if (!dir_in && len > 0) memcpy((uint8_t *)u->ctrl_buf + off, data, len);
    while (remaining > 0) {
        int chunk = remaining > mp ? mp : remaining;
        td[n].ctrl_status = TD_ACTIVE | TD_CERR3 | ls | TD_SPD;
        td[n].token = mk_token(dir_in ? PID_IN : PID_OUT, addr, 0, toggle, chunk);
        td[n].buffer = (uint32_t)(u->ctrl_buf_phys + off);
        off += chunk;
        remaining -= chunk;
        toggle ^= 1;
        n++;
        if (n >= 30) break;
    }

    /* STATUS (opposite direction, DATA1, IOC). */
    td[n].ctrl_status = TD_ACTIVE | TD_CERR3 | ls | TD_IOC;
    td[n].token = mk_token(dir_in ? PID_OUT : PID_IN, addr, 0, 1, 0);
    td[n].buffer = 0;
    n++;

    /* Link the chain depth-first. */
    for (int i = 0; i < n; i++) {
        if (i == n - 1)
            td[i].link = LP_TERMINATE;
        else
            td[i].link = (uint32_t)(u->ctrl_td_phys + (i + 1) * sizeof(struct uhci_td)) | LP_VF;
    }

    if (uhci_run_ctrl(u, u->ctrl_td_phys) < 0)
        return -1;

    /* Compute bytes transferred for IN: sum ActLen of data TDs. */
    int transferred = 0;
    if (dir_in) {
        int idx = 1; /* data TDs start after SETUP */
        int rem = len;
        while (rem > 0 && idx < n - 1) {
            int act = (td[idx].ctrl_status & TD_ACTLEN_MASK);
            act = (act == 0x7FF) ? 0 : act + 1; /* ActLen encoding */
            int chunk = rem > mp ? mp : rem;
            if (act > chunk) act = chunk;
            transferred += act;
            rem -= chunk;
            idx++;
        }
        if (data && transferred > 0)
            memcpy(data, (uint8_t *)u->ctrl_buf + 64, transferred);
    }
    return dir_in ? transferred : len;
}

/* --- ports / enumeration --- */
static int uhci_address(struct usb_hcd *hcd, struct usb_device *dev);

static int uhci_num_ports(struct usb_hcd *hcd) { (void)hcd; return 2; }

static int uhci_port_connected(struct usb_hcd *hcd, int port) {
    struct uhci *u = hcd->priv;
    uint16_t sc = hal_inw(u->io + UHCI_PORTSC1 + port * 2);
    return (sc & PORT_CCS) ? 1 : 0;
}

static int uhci_enumerate_port(struct usb_hcd *hcd, int port, struct usb_device *dev) {
    struct uhci *u = hcd->priv;
    uint16_t p = u->io + UHCI_PORTSC1 + port * 2;
    uint16_t sc = hal_inw(p);
    if (!(sc & PORT_CCS)) return -1;

    int low_speed = (sc & PORT_LS) ? 1 : 0;

    /* Reset sequence: assert PR, hold, deassert, then enable. */
    hal_outw(p, sc | PORT_PR);
    for (long s = 0; s < 5000000L; s++) { /* ~hold */ }
    sc = hal_inw(p);
    hal_outw(p, sc & ~PORT_PR);
    for (long s = 0; s < 2000000L; s++) { }
    /* enable port, clear change bits */
    hal_outw(p, (hal_inw(p) | PORT_PED) );
    for (long s = 0; s < 2000000L; s++) { }
    /* clear CSC/PEDC (write 1 to clear) */
    hal_outw(p, hal_inw(p) | PORT_CSC | PORT_PEDC);
    if (!(hal_inw(p) & PORT_PED)) return -1;

    dev->speed = low_speed ? USB_SPEED_LOW : USB_SPEED_FULL;
    dev->max_packet0 = 8;
    return uhci_address(hcd, dev);
}

/*
 * Assign an address on the default pipe (addr 0). Used both for root-port
 * devices and hub children — for the latter the hub already reset/enabled the
 * port, so this is the only step the controller owns. Only one device may sit
 * at address 0 at a time, which the sequential enumeration guarantees.
 */
static int uhci_address(struct usb_hcd *hcd, struct usb_device *dev) {
    struct uhci *u = hcd->priv;
    dev->addr = 0;

    struct usb_device_descriptor dd;
    memset(&dd, 0, sizeof(dd));
    struct usb_setup s8 = { USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, (USB_DT_DEVICE << 8), 0, 8 };
    if (uhci_control(hcd, dev, &s8, &dd, 8) < 8) return -1;
    if (dd.bMaxPacketSize0) dev->max_packet0 = dd.bMaxPacketSize0;

    uint8_t new_addr = ++u->next_addr;
    struct usb_setup sa = { USB_DIR_OUT, USB_REQ_SET_ADDRESS, new_addr, 0, 0 };
    if (uhci_control(hcd, dev, &sa, NULL, 0) < 0) return -1;
    dev->addr = new_addr;
    for (long s = 0; s < 2000000L; s++) { } /* SET_ADDRESS recovery */
    return 0;
}

static int uhci_address_dev(struct usb_hcd *hcd, struct usb_device *dev) {
    return uhci_address(hcd, dev);
}

/* --- interrupt IN endpoint --- */
static int uhci_intr_open(struct usb_hcd *hcd, struct usb_device *dev,
                          uint8_t ep_addr, int max_packet, int interval) {
    (void)interval;
    struct uhci *u = hcd->priv;
    if (u->ep_count >= (int)(sizeof(u->eps) / sizeof(u->eps[0]))) return -1;
    struct uhci_ep *e = &u->eps[u->ep_count];
    memset(e, 0, sizeof(*e));

    e->qh = (struct uhci_qh *)page_va(&e->qh_phys);
    e->td = (struct uhci_td *)page_va(&e->td_phys);
    e->buf = page_va(&e->buf_phys);
    if (!e->qh || !e->td || !e->buf) return -1;
    e->maxlen = max_packet;
    e->low_speed = (dev->speed == USB_SPEED_LOW);
    e->addr = dev->addr;
    e->ep = ep_addr & 0x0F;
    e->toggle = 0;
    e->active = 1;
    dev->hcd_priv = e;

    /* Arm the first IN TD. */
    e->td->ctrl_status = TD_ACTIVE | TD_CERR3 | TD_IOC | (e->low_speed ? TD_LS : 0);
    e->td->token = mk_token(PID_IN, e->addr, e->ep, e->toggle, e->maxlen);
    e->td->buffer = (uint32_t)e->buf_phys;
    e->td->link = LP_TERMINATE;
    e->qh->elem = (uint32_t)e->td_phys;

    /* Chain this QH at the head of the schedule, before the control QH. */
    e->qh->head = (uint32_t)u->qh_ctrl_phys | LP_QH;
    if (u->ep_count > 0)
        u->eps[u->ep_count - 1].qh->head = (uint32_t)e->qh_phys | LP_QH;
    uint32_t first = (uint32_t)e->qh_phys | LP_QH;
    /* If this is the first interrupt EP, point the frame list at it. */
    if (u->ep_count == 0) {
        for (int i = 0; i < 1024; i++) u->frame[i] = first;
    }
    u->ep_count++;
    return 0;
}

static int uhci_intr_poll(struct usb_hcd *hcd, struct usb_device *dev,
                          uint8_t ep_addr, void *buf, int len) {
    (void)hcd; (void)ep_addr;
    struct uhci_ep *e = dev->hcd_priv;
    if (!e || !e->active) return 0;
    if (e->td->ctrl_status & TD_ACTIVE) return 0; /* still pending */

    int act = e->td->ctrl_status & TD_ACTLEN_MASK;
    act = (act == 0x7FF) ? 0 : act + 1;
    int stalled = (e->td->ctrl_status & (1u << 22)) != 0;
    int got = 0;
    if (!stalled && act > 0) {
        got = act > len ? len : act;
        memcpy(buf, e->buf, got);
        e->toggle ^= 1;
    }
    /* Re-arm. */
    e->td->ctrl_status = TD_ACTIVE | TD_CERR3 | TD_IOC | (e->low_speed ? TD_LS : 0);
    e->td->token = mk_token(PID_IN, e->addr, e->ep, e->toggle, e->maxlen);
    e->td->buffer = (uint32_t)e->buf_phys;
    e->td->link = LP_TERMINATE;
    e->qh->elem = (uint32_t)e->td_phys;
    return got;
}

static struct usb_hcd_ops uhci_ops = {
    .num_ports = uhci_num_ports,
    .port_connected = uhci_port_connected,
    .enumerate_port = uhci_enumerate_port,
    .address_dev = uhci_address_dev,
    .control = uhci_control,
    .intr_open = uhci_intr_open,
    .intr_poll = uhci_intr_poll,
};

static int uhci_probe(struct hal_device *dev) {
    if (uhci_count >= (int)(sizeof(uhci_ctrls) / sizeof(uhci_ctrls[0]))) return -1;
    struct uhci *u = &uhci_ctrls[uhci_count];
    memset(u, 0, sizeof(*u));

    /* BAR4 is the UHCI I/O BAR on PIIX/ICH; fall back to dev->base. */
    uint32_t bar4 = pci_get_bar(dev->pci_bdf, 4);
    uint16_t io = (uint16_t)((bar4 & 1) ? (bar4 & ~0x3u) : (dev->base & 0xFFFF));
    if (!io) return -1;
    u->io = io;

    pr_info("UHCI: %s io=0x%04x\n", dev->name, io);

    /* Global + host reset. */
    hal_outw(u->io + UHCI_USBCMD, CMD_GRESET);
    for (long s = 0; s < 2000000L; s++) { }
    hal_outw(u->io + UHCI_USBCMD, 0);
    hal_outw(u->io + UHCI_USBCMD, CMD_HCRESET);
    for (long s = 0; s < 5000000L; s++)
        if (!(hal_inw(u->io + UHCI_USBCMD) & CMD_HCRESET)) break;

    /* Frame list + skeleton control QH + control TD pool + bounce. */
    u->frame = (uint32_t *)page_va(&u->frame_phys);
    u->qh_ctrl = (struct uhci_qh *)page_va(&u->qh_ctrl_phys);
    u->ctrl_td = (struct uhci_td *)page_va(&u->ctrl_td_phys);
    u->ctrl_buf = page_va(&u->ctrl_buf_phys);
    if (!u->frame || !u->qh_ctrl || !u->ctrl_td || !u->ctrl_buf) return -1;

    u->qh_ctrl->head = LP_TERMINATE;
    u->qh_ctrl->elem = LP_TERMINATE;
    for (int i = 0; i < 1024; i++)
        u->frame[i] = (uint32_t)u->qh_ctrl_phys | LP_QH;

    hal_outl(u->io + UHCI_FRBASEADD, (uint32_t)u->frame_phys);
    hal_outw(u->io + UHCI_FRNUM, 0);
    hal_outb(u->io + UHCI_SOFMOD, 64);
    hal_outw(u->io + UHCI_USBINTR, 0); /* polled, no IRQ */
    hal_outw(u->io + UHCI_USBCMD, CMD_RUN | CMD_MAXP);

    u->hcd.name = "UHCI";
    u->hcd.ops = &uhci_ops;
    u->hcd.priv = u;
    uhci_count++;

    usb_register_hcd(&u->hcd);
    return 0;
}

static struct device_driver uhci_driver = {
    .name = "uhci",
    .vendor = DRV_ANY_ID,
    .device = DRV_ANY_ID,
    .class_code = 0x0C,
    .subclass = 0x03,
    .prog_if = 0x00, /* UHCI */
    .probe = uhci_probe,
};

void uhci_register(void) {
    driver_register(&uhci_driver);
}
