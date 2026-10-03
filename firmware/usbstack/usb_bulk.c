/*
 * Vendor-specific bulk interface for the iPod OS companion link.
 *
 * CDC-ACM is claimed by the host's serial tty layer, which fragments bulk writes into
 * ~100-200 byte transfers and caps host-to-device throughput at well under 1 MB/s. A
 * vendor-class interface is handed straight to WebUSB, so the browser's bulk transfers
 * reach the endpoint whole.
 *
 * The data path mirrors usb_serial.c: byte rings with monotonic head/tail counters, and
 * two receive buffers so the next transfer is armed before the finished one is copied out.
 */
#include "string.h"
#include "system.h"
#include "usb_core.h"
#include "usb_drv.h"
#include "kernel.h"
#include "usb_bulk.h"
#include "usb_class_driver.h"

/*#define LOGF_ENABLE*/
#include "logf.h"

#define XFER_MAX    65536
#define RX_XFER     65536
#define TX_RING     (128 * 1024)
#define RX_RING     (256 * 1024)

static unsigned char tx_ring[TX_RING];
static unsigned char rx_ring[RX_RING];
static unsigned char tx_xfer[XFER_MAX] USB_DEVBSS_ATTR __attribute__((aligned(32)));
static unsigned char rx_xfer_buf[2][RX_XFER] USB_DEVBSS_ATTR __attribute__((aligned(32)));

static size_t tx_head, tx_tail, tx_inflight;
static size_t rx_head, rx_tail;
static int rx_cur;
static bool rx_armed;
static bool active;

static struct usb_bulk_stats stats;
static unsigned long rx_mark;

static unsigned long bulk_now_us(void)
{
#ifdef USEC_TIMER
    return USEC_TIMER;
#else
    return (unsigned long)current_tick * (1000000 / HZ);
#endif
}

static struct usb_interface_descriptor __attribute__((aligned(2))) interface_descriptor =
{
    .bLength            = sizeof(struct usb_interface_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = 0,
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 2,
    .bInterfaceClass    = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass = USB_BULK_SUBCLASS,
    .bInterfaceProtocol = USB_BULK_PROTOCOL,
    .iInterface         = 0
};

static struct usb_endpoint_descriptor __attribute__((aligned(2))) endpoint_descriptor =
{
    .bLength            = sizeof(struct usb_endpoint_descriptor),
    .bDescriptorType    = USB_DT_ENDPOINT,
    .bEndpointAddress   = 0,
    .bmAttributes       = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize     = 0,
    .bInterval          = 0
};

static struct usb_class_driver_ep_allocation ep_allocs[2] = {
    {.type = USB_ENDPOINT_XFER_BULK, .dir = DIR_IN, .optional = false, .mps = -1},
    {.type = USB_ENDPOINT_XFER_BULK, .dir = DIR_OUT, .optional = false, .mps = -1},
};

#define EP_IN (ep_allocs[0].ep)
#define EP_OUT (ep_allocs[1].ep)

static int usb_interface;

int usb_bulk_interface(void)
{
    return usb_interface;
}

static int usb_bulk_set_first_interface(int interface)
{
    usb_interface = interface;
    return interface + 1;
}

static int usb_bulk_get_config_descriptor(unsigned char *dest, int max_packet_size)
{
    unsigned char *orig_dest = dest;

    interface_descriptor.bInterfaceNumber = usb_interface;
    PACK_DATA(&dest, interface_descriptor);

    endpoint_descriptor.wMaxPacketSize = max_packet_size;
    endpoint_descriptor.bEndpointAddress = EP_IN;
    PACK_DATA(&dest, endpoint_descriptor);

    endpoint_descriptor.bEndpointAddress = EP_OUT;
    PACK_DATA(&dest, endpoint_descriptor);

    return (dest - orig_dest);
}

static void tx_kick(void)
{
    if (!active || tx_inflight)
        return;
    size_t used = tx_head - tx_tail;
    if (!used)
        return;
    size_t off = tx_tail % TX_RING;
    size_t n = MIN(used, TX_RING - off);
    n = MIN(n, (size_t)XFER_MAX);
    /* End each transfer with a short packet so the host's read completes without
     * waiting for a zero-length packet. */
    size_t mps = usb_drv_port_speed() ? 512 : 64;
    if (n >= mps && n % mps == 0)
        n--;
    memcpy(tx_xfer, &tx_ring[off], n);
    tx_inflight = n;
    usb_drv_send_nonblocking(EP_IN, tx_xfer, n);
}

static void rx_start(void)
{
    unsigned long now = bulk_now_us();
    stats.rx_idle_us += now - rx_mark;
    rx_mark = now;
    rx_armed = true;
    usb_drv_recv_nonblocking(EP_OUT, rx_xfer_buf[rx_cur], RX_XFER);
}

static void rx_arm(void)
{
    if (!active || rx_armed)
        return;
    if (RX_RING - (rx_head - rx_tail) < RX_XFER) {
        stats.rx_stalls++;
        return; /* resumes from usb_bulk_read() once there is room */
    }
    rx_start();
}

static void rx_copy(const unsigned char *src, size_t n)
{
    size_t off = rx_head % RX_RING;
    size_t first = MIN(n, RX_RING - off);
    memcpy(&rx_ring[off], src, first);
    memcpy(rx_ring, src + first, n - first);
    rx_head += n;
}

static int usb_bulk_init_connection(void)
{
    tx_inflight = 0;
    rx_armed = false;
    rx_head = rx_tail = 0;
    rx_cur = 0;
    memset(&stats, 0, sizeof stats);
    rx_mark = bulk_now_us();
    active = true;
    rx_arm();
    tx_kick();
    return 0;
}

static void usb_bulk_init(void)
{
    tx_head = tx_tail = tx_inflight = 0;
    rx_head = rx_tail = 0;
    rx_armed = false;
    active = false;
}

static void usb_bulk_disconnect(void)
{
    active = false;
    rx_armed = false;
    tx_inflight = 0;
    tx_tail = tx_head;
}

bool usb_bulk_connected(void)
{
    return active;
}

void usb_bulk_get_stats(struct usb_bulk_stats *out)
{
    *out = stats;
    out->rx_xfer_size = RX_XFER;
}

int usb_bulk_read(void *buf, int maxlen)
{
    unsigned char *out = buf;
    size_t used = rx_head - rx_tail;
    size_t n = MIN(used, (size_t)(maxlen > 0 ? maxlen : 0));
    size_t off = rx_tail % RX_RING;
    size_t first = MIN(n, RX_RING - off);
    memcpy(out, &rx_ring[off], first);
    memcpy(out + first, rx_ring, n - first);
    rx_tail += n;
    rx_arm();
    return (int)n;
}

int usb_bulk_write(const void *data, int length)
{
    const unsigned char *p = data;
    int left = length;
    while (left > 0)
    {
        if (!active)
            return -1;
        size_t space = TX_RING - (tx_head - tx_tail);
        if (!space)
        {
            tx_kick();
            yield();
            continue;
        }
        size_t off = tx_head % TX_RING;
        size_t n = MIN(space, TX_RING - off);
        n = MIN(n, (size_t)left);
        memcpy(&tx_ring[off], p, n);
        tx_head += n;
        p += n;
        left -= (int)n;
        tx_kick();
    }
    return length;
}

static void usb_bulk_transfer_complete(int ep, int dir, int status, int length)
{
    (void)ep;

    switch (dir) {
        case USB_DIR_OUT: {
            unsigned long now = bulk_now_us();
            stats.rx_busy_us += now - rx_mark;
            rx_mark = now;
            rx_armed = false;
            int done = rx_cur;
            size_t n = (status == 0 && length > 0) ? MIN((size_t)length, (size_t)RX_XFER) : 0;
            stats.rx_xfers++;
            stats.rx_bytes += n;
            if (status != 0)
                stats.rx_errors++;
            /* Arm the next transfer into the other buffer before copying this one out. */
            if (active && RX_RING - (rx_head - rx_tail) >= n + RX_XFER)
            {
                rx_cur ^= 1;
                rx_start();
            }
            rx_copy(rx_xfer_buf[done], n);
            rx_arm(); /* no-op when already armed above */
            break;
        }

        case USB_DIR_IN:
            if (status == 0)
                tx_tail += tx_inflight;
            tx_inflight = 0;
            tx_kick();
            break;
    }
}

struct usb_class_driver usb_cdrv_bulk = {
    .needs_exclusive_storage = false,
    .needs_cpu_boost = true,
    .config = 1,
    .ep_allocs_size = ARRAYLEN(ep_allocs),
    .ep_allocs = ep_allocs,
    .set_first_interface = usb_bulk_set_first_interface,
    .get_config_descriptor = usb_bulk_get_config_descriptor,
    .init_connection = usb_bulk_init_connection,
    .init = usb_bulk_init,
    .disconnect = usb_bulk_disconnect,
    .transfer_complete = usb_bulk_transfer_complete,
};
