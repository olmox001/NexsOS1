/*
 * kernel/drivers/usb/ehci.c
 * EHCI (USB 2.0) host controller driver — polled.
 *
 * EHCI only drives high-speed devices directly; low/full-speed devices (USB
 * boot keyboards/mice) are handed to a companion UHCI/OHCI by setting the port
 * owner. So this driver's two jobs are: (1) claim the ports, reset each one and
 * route non-high-speed devices to the companion (which our UHCI driver then
 * enumerates), and (2) run an async (control) + periodic (interrupt) schedule
 * for any genuine high-speed device. Polled, like xHCI/UHCI.
 */

#include <drivers/usb/usb.h>
#include <drivers/pci.h>
#include <kernel/driver.h>
#include <kernel/hal.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>

extern int arch_vmm_map_device(uint64_t base, uint64_t size);

/* Capability registers (from MMIO base). */
#define EHCI_CAPLENGTH 0x00 /* u8 */
#define EHCI_HCSPARAMS 0x04

/* Operational registers (from base + CAPLENGTH). */
#define EHCI_USBCMD    0x00
#define EHCI_USBSTS    0x04
#define EHCI_USBINTR   0x08
#define EHCI_FRINDEX   0x0C
#define EHCI_CTRLDSSEG 0x10
#define EHCI_PERIODICLB 0x14
#define EHCI_ASYNCLB   0x18
#define EHCI_CONFIGFLAG 0x40
#define EHCI_PORTSC(n) (0x44 + 4 * (n))

#define CMD_RUN     (1u << 0)
#define CMD_HCRESET (1u << 1)
#define CMD_PSE     (1u << 4)
#define CMD_ASE     (1u << 5)
#define STS_HALTED  (1u << 12)

#define PORT_CCS   (1u << 0)
#define PORT_CSC   (1u << 1)
#define PORT_PED   (1u << 2)
#define PORT_PEDC  (1u << 3)
#define PORT_RESET (1u << 8)
#define PORT_POWER (1u << 12)
#define PORT_OWNER (1u << 13)
#define PORT_LS_KSTATE (1u << 10) /* line status bit: low-speed (K-state) */

/* qTD token. */
#define QTD_ACTIVE (1u << 7)
#define QTD_HALTED (1u << 6)
#define QTD_PID_OUT   0
#define QTD_PID_IN    1
#define QTD_PID_SETUP 2
#define QTD_IOC    (1u << 15)
#define QTD_T      1u /* terminate */

/* Link pointer type. */
#define LP_QH (1u << 1)
#define LP_T  1u

/* Exactly 32 bytes: qTDs are packed in a pool and the hardware requires each to
 * be 32-byte aligned, so the struct size must stay a multiple of 32. */
struct ehci_qtd {
    volatile uint32_t next;
    volatile uint32_t alt_next;
    volatile uint32_t token;
    volatile uint32_t buf[5];
} __attribute__((packed));

struct ehci_qh {
    volatile uint32_t horiz;
    volatile uint32_t ep_char;   /* addr, ep, EPS, max packet, H, DTC */
    volatile uint32_t ep_cap;    /* mult, hub addr/port, s/c-mask */
    volatile uint32_t cur_qtd;
    /* qTD overlay */
    volatile uint32_t ov_next;
    volatile uint32_t ov_alt;
    volatile uint32_t ov_token;
    volatile uint32_t ov_buf[5];
    uint32_t pad[4]; /* pad to 64B (48B used) */
} __attribute__((packed));

struct ehci_ep {
    struct ehci_qh *qh;
    uint64_t qh_phys;
    struct ehci_qtd *qtd;
    uint64_t qtd_phys;
    void *buf;
    uint64_t buf_phys;
    int toggle;
    int maxlen;
    int active;
};

struct ehci {
    volatile uint8_t *base;
    volatile uint8_t *op;
    int nports;
    uint32_t *framelist;
    uint64_t framelist_phys;
    struct ehci_qh *async; /* async head QH (H=1) */
    uint64_t async_phys;

    struct ehci_qh *ctrl_qh;     /* reused per control transfer */
    uint64_t ctrl_qh_phys;
    struct ehci_qtd *ctrl_qtd;   /* pool */
    uint64_t ctrl_qtd_phys;
    void *ctrl_buf;
    uint64_t ctrl_buf_phys;

    struct ehci_ep eps[4];
    int ep_count;
    uint8_t next_addr;
    struct usb_hcd hcd;
};

static struct ehci ehci_ctrls[2];
static int ehci_count;

static inline uint32_t rd32(volatile uint8_t *p, uint32_t o) { return *(volatile uint32_t *)(p + o); }
static inline void wr32(volatile uint8_t *p, uint32_t o, uint32_t v) { *(volatile uint32_t *)(p + o) = v; }

static void *page_va(uint64_t *phys_out) {
    if (phys_out) *phys_out = 0;
    void *p = pmm_alloc_page();
    if (!p) return NULL;
    memset(p, 0, PAGE_SIZE);
    if (phys_out) *phys_out = virt_to_phys(p);
    return p;
}

static int ehci_address(struct usb_hcd *hcd, struct usb_device *dev);

/* Fill a qTD: pid, data toggle, length, buffer phys, IOC. */
static void qtd_fill(struct ehci_qtd *t, uint32_t next, int pid, int toggle,
                     int len, uint64_t buf, int ioc) {
    t->next = next;
    t->alt_next = LP_T;
    uint32_t tok = QTD_ACTIVE | ((uint32_t)pid << 8) | (3u << 10) /* CERR */ |
                   ((uint32_t)len << 16) | ((uint32_t)(toggle & 1) << 31);
    if (ioc) tok |= QTD_IOC;
    t->token = tok;
    for (int i = 0; i < 5; i++) t->buf[i] = 0;
    if (len) {
        t->buf[0] = (uint32_t)buf;
        t->buf[1] = (uint32_t)((buf + 0x1000) & ~0xFFFUL);
    }
}

/* Run a qTD chain (starting at ctrl_qtd[0]) through the control QH and wait for
 * the STATUS qTD to retire. We must poll the qTD itself, not the QH overlay:
 * the overlay starts inactive (so the controller fetches the first qTD), so
 * polling it would report "done" immediately. Returns 0 on success. */
static int ehci_run_ctrl(struct ehci *e, struct usb_device *dev, struct ehci_qtd *last) {
    struct ehci_qh *qh = e->ctrl_qh;
    int eps = (dev->speed == USB_SPEED_HIGH) ? 2 : (dev->speed == USB_SPEED_LOW) ? 1 : 0;
    int mp = dev->max_packet0 ? dev->max_packet0 : 8;
    qh->ep_char = (uint32_t)dev->addr | ((uint32_t)0 << 8) | ((uint32_t)eps << 12) |
                  (1u << 14) /* DTC: toggle from qTD */ | ((uint32_t)mp << 16) |
                  ((eps != 2) ? (1u << 27) : 0) /* control ep on non-HS */;
    qh->ep_cap = (1u << 30); /* mult = 1 */
    qh->cur_qtd = 0;
    qh->ov_next = (uint32_t)e->ctrl_qtd_phys; /* first qTD */
    qh->ov_alt = LP_T;
    qh->ov_token = 0; /* inactive -> controller loads the first qTD */

    for (long s = 0; s < 60000000L; s++) {
        if (!(last->token & QTD_ACTIVE)) break;
    }
    if (last->token & (QTD_ACTIVE | QTD_HALTED)) return -1;
    return 0;
}

static int ehci_control(struct usb_hcd *hcd, struct usb_device *dev,
                        const struct usb_setup *setup, void *data, int len) {
    struct ehci *e = hcd->priv;
    struct ehci_qtd *td = e->ctrl_qtd;
    int dir_in = (setup->bmRequestType & USB_DIR_IN) != 0;
    int n = 0;

    /* SETUP (DATA0). */
    memcpy(e->ctrl_buf, setup, 8);
    int last = (len > 0) ? 2 : 1; /* index of STATUS stage */
    qtd_fill(&td[0], e->ctrl_qtd_phys + 1 * sizeof(struct ehci_qtd),
             QTD_PID_SETUP, 0, 8, e->ctrl_buf_phys, 0);
    n = 1;

    if (len > 0) {
        if (!dir_in) memcpy((uint8_t *)e->ctrl_buf + 64, data, len);
        qtd_fill(&td[1], e->ctrl_qtd_phys + 2 * sizeof(struct ehci_qtd),
                 dir_in ? QTD_PID_IN : QTD_PID_OUT, 1, len, e->ctrl_buf_phys + 64, 0);
        n = 2;
    }

    /* STATUS (DATA1, opposite dir, IOC). */
    qtd_fill(&td[last], LP_T, dir_in ? QTD_PID_OUT : QTD_PID_IN, 1, 0, 0, 1);
    n = last + 1;
    (void)n;

    if (ehci_run_ctrl(e, dev, &td[last]) < 0) return -1;

    int transferred = len;
    if (dir_in && len > 0) {
        int rem = (td[1].token >> 16) & 0x7FFF; /* bytes NOT transferred */
        transferred = len - rem;
        if (transferred < 0) transferred = 0;
        if (data && transferred > 0) memcpy(data, (uint8_t *)e->ctrl_buf + 64, transferred);
    }
    return transferred;
}

/* --- ports --- */
static int ehci_num_ports(struct usb_hcd *hcd) {
    struct ehci *e = hcd->priv;
    return e->nports;
}
static int ehci_port_connected(struct usb_hcd *hcd, int port) {
    struct ehci *e = hcd->priv;
    return (rd32(e->op, EHCI_PORTSC(port)) & PORT_CCS) ? 1 : 0;
}

static int ehci_enumerate_port(struct usb_hcd *hcd, int port, struct usb_device *dev) {
    struct ehci *e = hcd->priv;
    uint32_t sc = rd32(e->op, EHCI_PORTSC(port));
    if (!(sc & PORT_CCS)) return -1;

    /* Reset to determine speed. A genuine high-speed device ends up enabled
     * (PED) and is driven here; anything else (full/low-speed boot keyboard or
     * mouse) is released to the companion UHCI/OHCI via the port-owner bit. */
    wr32(e->op, EHCI_PORTSC(port), (sc & ~0x2Au) | PORT_RESET);
    for (long s = 0; s < 8000000L; s++) { }
    wr32(e->op, EHCI_PORTSC(port), rd32(e->op, EHCI_PORTSC(port)) & ~PORT_RESET);
    for (long s = 0; s < 4000000L; s++) {
        if (!(rd32(e->op, EHCI_PORTSC(port)) & PORT_RESET)) break;
    }
    sc = rd32(e->op, EHCI_PORTSC(port));

    if (!(sc & PORT_PED)) {
        /* Not enabled after reset = full/low-speed: hand to the companion. */
        wr32(e->op, EHCI_PORTSC(port), (sc & ~0x2Au) | PORT_OWNER);
        return -1;
    }

    dev->speed = USB_SPEED_HIGH;
    dev->max_packet0 = 64;
    return ehci_address(hcd, dev);
}

/* Address a device on the default pipe (addr 0). */
static int ehci_address(struct usb_hcd *hcd, struct usb_device *dev) {
    dev->addr = 0;
    struct usb_device_descriptor dd;
    memset(&dd, 0, sizeof(dd));
    struct usb_setup s8 = { USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, (USB_DT_DEVICE << 8), 0, 8 };
    if (ehci_control(hcd, dev, &s8, &dd, 8) < 8) return -1;
    if (dd.bMaxPacketSize0) dev->max_packet0 = dd.bMaxPacketSize0;

    struct ehci *e = hcd->priv;
    uint8_t new_addr = ++e->next_addr;
    struct usb_setup sa = { USB_DIR_OUT, USB_REQ_SET_ADDRESS, new_addr, 0, 0 };
    if (ehci_control(hcd, dev, &sa, NULL, 0) < 0) return -1;
    dev->addr = new_addr;
    for (long s = 0; s < 2000000L; s++) { }
    return 0;
}

static int ehci_address_dev(struct usb_hcd *hcd, struct usb_device *dev) {
    return ehci_address(hcd, dev);
}

/* --- interrupt IN endpoint (periodic schedule) --- */
static int ehci_intr_open(struct usb_hcd *hcd, struct usb_device *dev,
                          uint8_t ep_addr, int max_packet, int interval) {
    (void)interval;
    struct ehci *e = hcd->priv;
    if (e->ep_count >= (int)(sizeof(e->eps) / sizeof(e->eps[0]))) return -1;
    struct ehci_ep *ep = &e->eps[e->ep_count];
    memset(ep, 0, sizeof(*ep));
    ep->qh = (struct ehci_qh *)page_va(&ep->qh_phys);
    ep->qtd = (struct ehci_qtd *)page_va(&ep->qtd_phys);
    ep->buf = page_va(&ep->buf_phys);
    if (!ep->qh || !ep->qtd || !ep->buf) return -1;
    ep->maxlen = max_packet;
    ep->toggle = 0;
    ep->active = 1;
    dev->hcd_priv = ep;

    int epn = ep_addr & 0x0F;
    ep->qh->ep_char = (uint32_t)dev->addr | ((uint32_t)epn << 8) | (2u << 12) /*HS*/ |
                      (1u << 14) | ((uint32_t)max_packet << 16);
    ep->qh->ep_cap = (1u << 30) | (0x01u << 0) /* S-mask: microframe 0 */;
    ep->qh->horiz = LP_T;
    qtd_fill(ep->qtd, LP_T, QTD_PID_IN, ep->toggle, max_packet, ep->buf_phys, 1);
    ep->qh->ov_next = (uint32_t)ep->qtd_phys;
    ep->qh->ov_alt = LP_T;
    ep->qh->ov_token = 0;

    /* Link the interrupt QH into every periodic frame-list entry. */
    uint32_t link = (uint32_t)ep->qh_phys | LP_QH;
    for (int i = 0; i < 1024; i++) e->framelist[i] = link;
    e->ep_count++;
    return 0;
}

static int ehci_intr_poll(struct usb_hcd *hcd, struct usb_device *dev,
                          uint8_t ep_addr, void *buf, int len) {
    (void)hcd; (void)ep_addr;
    struct ehci_ep *ep = dev->hcd_priv;
    if (!ep || !ep->active) return 0;
    if (ep->qh->ov_token & QTD_ACTIVE) return 0;

    int rem = (ep->qh->ov_token >> 16) & 0x7FFF;
    int halted = (ep->qh->ov_token & QTD_HALTED) != 0;
    int got = 0;
    if (!halted) {
        got = ep->maxlen - rem;
        if (got > len) got = len;
        if (got > 0) memcpy(buf, ep->buf, got);
        ep->toggle ^= 1;
    }
    /* Re-arm. */
    qtd_fill(ep->qtd, LP_T, QTD_PID_IN, ep->toggle, ep->maxlen, ep->buf_phys, 1);
    ep->qh->ov_next = (uint32_t)ep->qtd_phys;
    ep->qh->ov_alt = LP_T;
    ep->qh->ov_token = 0;
    return got;
}

static struct usb_hcd_ops ehci_ops = {
    .num_ports = ehci_num_ports,
    .port_connected = ehci_port_connected,
    .enumerate_port = ehci_enumerate_port,
    .address_dev = ehci_address_dev,
    .control = ehci_control,
    .intr_open = ehci_intr_open,
    .intr_poll = ehci_intr_poll,
};

static int ehci_probe(struct hal_device *dev) {
    if (ehci_count >= (int)(sizeof(ehci_ctrls) / sizeof(ehci_ctrls[0]))) return -1;
    struct ehci *e = &ehci_ctrls[ehci_count];
    memset(e, 0, sizeof(*e));

    uint64_t base = dev->base;
    if (!base) return -1;
    arch_vmm_map_device(base, 0x1000);
    e->base = (volatile uint8_t *)phys_to_virt(base);
    uint8_t caplen = *(volatile uint8_t *)(e->base + EHCI_CAPLENGTH);
    e->op = e->base + caplen;
    e->nports = rd32(e->base, EHCI_HCSPARAMS) & 0x0F;

    pr_info("EHCI: %s base=0x%lx ports=%d\n", dev->name, base, e->nports);

    /* Reset. */
    wr32(e->op, EHCI_USBCMD, rd32(e->op, EHCI_USBCMD) & ~CMD_RUN);
    for (long s = 0; s < 4000000L; s++)
        if (rd32(e->op, EHCI_USBSTS) & STS_HALTED) break;
    wr32(e->op, EHCI_USBCMD, CMD_HCRESET);
    for (long s = 0; s < 8000000L; s++)
        if (!(rd32(e->op, EHCI_USBCMD) & CMD_HCRESET)) break;

    /* Async head QH (H=1, points to itself). */
    e->async = (struct ehci_qh *)page_va(&e->async_phys);
    e->ctrl_qh = (struct ehci_qh *)page_va(&e->ctrl_qh_phys);
    e->ctrl_qtd = (struct ehci_qtd *)page_va(&e->ctrl_qtd_phys);
    e->ctrl_buf = page_va(&e->ctrl_buf_phys);
    e->framelist = (uint32_t *)page_va(&e->framelist_phys);
    if (!e->async || !e->ctrl_qh || !e->ctrl_qtd || !e->ctrl_buf || !e->framelist) return -1;

    /* async ring: head -> ctrl_qh -> head */
    e->async->ep_char = (1u << 15); /* H = 1 (head of reclamation) */
    e->async->ep_cap = (1u << 30);
    e->async->horiz = (uint32_t)e->ctrl_qh_phys | LP_QH;
    e->async->ov_next = LP_T;
    e->async->ov_token = QTD_HALTED;
    e->ctrl_qh->horiz = (uint32_t)e->async_phys | LP_QH;
    e->ctrl_qh->ov_next = LP_T;
    e->ctrl_qh->ov_token = QTD_HALTED;

    for (int i = 0; i < 1024; i++) e->framelist[i] = LP_T;

    wr32(e->op, EHCI_CTRLDSSEG, 0);
    wr32(e->op, EHCI_PERIODICLB, (uint32_t)e->framelist_phys);
    wr32(e->op, EHCI_ASYNCLB, (uint32_t)e->async_phys);
    wr32(e->op, EHCI_USBINTR, 0);
    wr32(e->op, EHCI_USBCMD, CMD_RUN | CMD_ASE | CMD_PSE | (8u << 16));
    wr32(e->op, EHCI_CONFIGFLAG, 1); /* route all ports to EHCI */
    for (long s = 0; s < 2000000L; s++) { }

    /* Power ports. */
    for (int p = 0; p < e->nports; p++) {
        uint32_t sc = rd32(e->op, EHCI_PORTSC(p));
        if (!(sc & PORT_POWER))
            wr32(e->op, EHCI_PORTSC(p), sc | PORT_POWER);
    }
    for (long s = 0; s < 3000000L; s++) { }

    e->hcd.name = "EHCI";
    e->hcd.ops = &ehci_ops;
    e->hcd.priv = e;
    ehci_count++;

    usb_register_hcd(&e->hcd);
    return 0;
}

static struct device_driver ehci_driver = {
    .name = "ehci",
    .vendor = DRV_ANY_ID,
    .device = DRV_ANY_ID,
    .class_code = 0x0C,
    .subclass = 0x03,
    .prog_if = 0x20, /* EHCI */
    .probe = ehci_probe,
};

void ehci_register(void) {
    driver_register(&ehci_driver);
}
