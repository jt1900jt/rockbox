/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 by Christian Gmeiner
 * Copyright (C) 2021 by Tomasz Moń
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "string.h"
#include "system.h"
#include "usb_core.h"
#include "usb_drv.h"
#include "kernel.h"
#include "usb_serial.h"
#include "usb_class_driver.h"
/*#define LOGF_ENABLE*/
#include "logf.h"

#define CDC_SUBCLASS_ACM         0x02
#define CDC_PROTOCOL_NONE        0x00

/* Class-Specific Request Codes */
#define SET_LINE_CODING          0x20
#define GET_LINE_CODING          0x21
#define SET_CONTROL_LINE_STATE   0x22

#define SUBTYPE_HEADER           0x00
#define SUBTYPE_CALL_MANAGEMENT  0x01
#define SUBTYPE_ACM              0x02
#define SUBTYPE_UNION            0x06

/* Support SET_LINE_CODING, GET_LINE_CODING, SET_CONTROL_LINE_STATE requests
 * and SERIAL_STATE notification.
 */
#define ACM_CAP_LINE_CODING      0x02

struct cdc_header_descriptor {
    uint8_t  bFunctionLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubtype;
    uint16_t bcdCDC;
} __attribute__((packed));

struct cdc_call_management_descriptor {
    uint8_t bFunctionLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bmCapabilities;
    uint8_t bDataInterface;
} __attribute__((packed));

struct cdc_acm_descriptor {
    uint8_t bFunctionLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bmCapabilities;
} __attribute__((packed));

struct cdc_union_descriptor {
    uint8_t  bFunctionLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubtype;
    uint8_t  bControlInterface;
    uint8_t  bSubordinateInterface0;
} __attribute__((packed));

struct cdc_line_coding {
    uint32_t dwDTERate;
    uint8_t bCharFormat;
    uint8_t bParityType;
    uint8_t bDataBits;
} __attribute__((packed));

static struct usb_interface_assoc_descriptor
    association_descriptor =
{
    .bLength            = sizeof(struct usb_interface_assoc_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE_ASSOCIATION,
    .bFirstInterface    = 0,
    .bInterfaceCount    = 2,
    .bFunctionClass     = USB_CLASS_COMM,
    .bFunctionSubClass  = CDC_SUBCLASS_ACM,
    .bFunctionProtocol  = CDC_PROTOCOL_NONE,
    .iFunction          = 0
};

static struct usb_interface_descriptor
    control_interface_descriptor =
{
    .bLength            = sizeof(struct usb_interface_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = 0,
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 1,
    .bInterfaceClass    = USB_CLASS_COMM,
    .bInterfaceSubClass = CDC_SUBCLASS_ACM,
    .bInterfaceProtocol = CDC_PROTOCOL_NONE,
    .iInterface         = 0
};

static struct cdc_header_descriptor
    header_descriptor =
{
    .bFunctionLength    = sizeof(struct cdc_header_descriptor),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubtype = SUBTYPE_HEADER,
    .bcdCDC             = 0x0110
};

static struct cdc_call_management_descriptor
    call_management_descriptor =
{
    .bFunctionLength    = sizeof(struct cdc_call_management_descriptor),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubtype = SUBTYPE_CALL_MANAGEMENT,
    .bmCapabilities     = 0,
    .bDataInterface     = 0
};

static struct cdc_acm_descriptor
    acm_descriptor =
{
    .bFunctionLength    = sizeof(struct cdc_acm_descriptor),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubtype = SUBTYPE_ACM,
    .bmCapabilities     = ACM_CAP_LINE_CODING
};

static struct cdc_union_descriptor
    union_descriptor =
{
    .bFunctionLength        = sizeof(struct cdc_union_descriptor),
    .bDescriptorType        = USB_DT_CS_INTERFACE,
    .bDescriptorSubtype     = SUBTYPE_UNION,
    .bControlInterface      = 0,
    .bSubordinateInterface0 = 0
};

static struct usb_interface_descriptor
    data_interface_descriptor =
{
    .bLength            = sizeof(struct usb_interface_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = 0,
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 2,
    .bInterfaceClass    = USB_CLASS_CDC_DATA,
    .bInterfaceSubClass = 0,
    .bInterfaceProtocol = 0,
    .iInterface         = 0
};

static struct usb_endpoint_descriptor
    endpoint_descriptor =
{
    .bLength          = sizeof(struct usb_endpoint_descriptor),
    .bDescriptorType  = USB_DT_ENDPOINT,
    .bEndpointAddress = 0,
    .bmAttributes     = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize   = 0,
    .bInterval        = 0
};

union line_coding_buffer
{
    struct cdc_line_coding data;
    unsigned char raw[64];
};

static union line_coding_buffer line_coding;

/* Data path.
 *
 * tx_ring/rx_ring are byte rings with monotonic head/tail counters (used = head - tail).
 * tx_xfer/rx_xfer are the DMA-able buffers handed to the USB driver.
 *
 * Transfer-complete callbacks run in the USB thread and readers/writers in other threads.
 * Rockbox threads are cooperative on these targets, so no locking is needed as long as
 * neither side yields mid-update. usb_serial_send() (logf) never blocks.
 *
 * PP502x needs boost for high speed USB transfers beyond ~100 bytes, so it keeps the
 * original 32-byte transfers.
 */
#ifdef CPU_PP
#define XFER_MAX    32
#define RX_XFER     512
#define TX_RING     4096
#define RX_RING     8192
#else
#define XFER_MAX    65536
#define RX_XFER     65536
#define TX_RING     (128 * 1024)
#define RX_RING     (256 * 1024)
#endif

static unsigned char tx_ring[TX_RING];
static unsigned char rx_ring[RX_RING];
static unsigned char tx_xfer[XFER_MAX] USB_DEVBSS_ATTR __attribute__((aligned(32)));
/* Two receive buffers: on completion the next transfer is armed into the other one before
 * the finished buffer is copied out, so the OUT endpoint is never left idle while the USB
 * thread works. Arming early needs room in the ring for both. */
static unsigned char rx_xfer_buf[2][RX_XFER] USB_DEVBSS_ATTR __attribute__((aligned(32)));

static size_t tx_head, tx_tail, tx_inflight;
static size_t rx_head, rx_tail;
static int rx_cur;
static bool rx_armed;
static bool active = false;

/* Diagnostics: how much of the time the OUT endpoint had a transfer armed. */
static struct usb_serial_stats stats;
static unsigned long rx_mark;

static unsigned long serial_now_us(void)
{
#ifdef USEC_TIMER
    return USEC_TIMER;
#else
    return (unsigned long)current_tick * (1000000 / HZ);
#endif
}

static struct usb_class_driver_ep_allocation ep_allocs[3] = {
    {.type = USB_ENDPOINT_XFER_BULK, .dir = DIR_IN, .optional = false, .mps = -1},
    {.type = USB_ENDPOINT_XFER_BULK, .dir = DIR_OUT, .optional = false, .mps = -1},
    {.type = USB_ENDPOINT_XFER_INT, .dir = DIR_IN, .optional = true, .mps = -1},
};

#define EP_IN (ep_allocs[0].ep)
#define EP_OUT (ep_allocs[1].ep)
#define EP_INT (ep_allocs[2].ep)

static int control_interface, data_interface;

static int usb_serial_set_first_interface(int interface)
{
    control_interface = interface;
    data_interface = interface + 1;
    return interface + 2;
}

static int usb_serial_get_config_descriptor(unsigned char *dest, int max_packet_size)
{
    unsigned char *orig_dest = dest;

    association_descriptor.bFirstInterface         = control_interface;
    control_interface_descriptor.bInterfaceNumber  = control_interface;
    call_management_descriptor.bDataInterface      = data_interface;
    union_descriptor.bControlInterface             = control_interface;
    union_descriptor.bSubordinateInterface0        = data_interface;
    data_interface_descriptor.bInterfaceNumber     = data_interface;

    if (EP_INT > 0)
    {
        PACK_DATA(&dest, association_descriptor);
        PACK_DATA(&dest, control_interface_descriptor);
        PACK_DATA(&dest, header_descriptor);
        PACK_DATA(&dest, call_management_descriptor);
        PACK_DATA(&dest, acm_descriptor);
        PACK_DATA(&dest, union_descriptor);

        /* Notification endpoint. Set wMaxPacketSize to 64 as it is valid
         * both on Full and High speed. Note that max_packet_size is for bulk.
         * Maximum bInterval for High Speed is 16 and for Full Speed is 255.
         */
        endpoint_descriptor.bEndpointAddress = EP_INT;
        endpoint_descriptor.bmAttributes     = USB_ENDPOINT_XFER_INT;
        endpoint_descriptor.wMaxPacketSize   = 64;
        endpoint_descriptor.bInterval        = 16;
        PACK_DATA(&dest, endpoint_descriptor);
    }

    PACK_DATA(&dest, data_interface_descriptor);
    endpoint_descriptor.bEndpointAddress = EP_IN;
    endpoint_descriptor.bmAttributes     = USB_ENDPOINT_XFER_BULK;
    endpoint_descriptor.wMaxPacketSize   = max_packet_size;
    endpoint_descriptor.bInterval        = 0;
    PACK_DATA(&dest, endpoint_descriptor);

    endpoint_descriptor.bEndpointAddress = EP_OUT;
    PACK_DATA(&dest, endpoint_descriptor);

    return (dest - orig_dest);
}

/* called by usb_core_control_request() */
static bool usb_serial_control_request(struct usb_ctrlrequest* req, uint8_t* reqdata, size_t reqdata_size)
{
    (void)reqdata_size; /* should check this? */

    bool handled = false;

    if (req->wIndex != control_interface)
    {
        return false;
    }

    if (req->bRequestType == (USB_DIR_OUT|USB_TYPE_CLASS|USB_RECIP_INTERFACE))
    {
        if (req->bRequest == SET_LINE_CODING)
        {
            if (req->wLength == sizeof(struct cdc_line_coding))
            {
                /* Receive line coding into local copy */
                memcpy(line_coding.raw, reqdata, sizeof(struct cdc_line_coding));
                usb_core_control_response(USB_CONTROL_ACK, NULL, 0);
                handled = true;
            }
        }
        else if (req->bRequest == SET_CONTROL_LINE_STATE)
        {
            if (req->wLength == 0)
            {
                /* wValue holds Control Signal Bitmap that is simply ignored here */
                usb_core_control_response(USB_CONTROL_ACK, NULL, 0);
                handled = true;
            }
        }
    }
    else if (req->bRequestType == (USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_INTERFACE))
    {
        if (req->bRequest == GET_LINE_CODING)
        {
            if (req->wLength == sizeof(struct cdc_line_coding))
            {
                /* Send back line coding so host is happy */
                usb_core_control_response(USB_CONTROL_ACK, line_coding.raw,
                                         sizeof(struct cdc_line_coding));
                handled = true;
            }
        }
    }

    return handled;
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
    /* End every transfer with a short packet so the host completes its read
     * without waiting for a zero-length packet. */
    size_t mps = usb_drv_port_speed() ? 512 : 64;
    if (n >= mps && n % mps == 0)
        n--;
    memcpy(tx_xfer, &tx_ring[off], n);
    tx_inflight = n;
    usb_drv_send_nonblocking(EP_IN, tx_xfer, n);
}

static void rx_start(void)
{
    unsigned long now = serial_now_us();
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
        return; /* resumes from usb_serial_read() once there is room */
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

void usb_serial_get_stats(struct usb_serial_stats *out)
{
    *out = stats;
    out->rx_xfer_size = RX_XFER;
}

static int usb_serial_init_connection(void)
{
    /* we come here too after a bus reset */
    tx_inflight = 0;
    rx_armed = false;
    rx_head = rx_tail = 0;
    rx_cur = 0;
    memset(&stats, 0, sizeof stats);
    rx_mark = serial_now_us();
    active = true;
    rx_arm();
    tx_kick();
    return 0;
}

/* called by usb_code_init() */
static void usb_serial_init(void)
{
    logf("serial: init");
    tx_head = tx_tail = tx_inflight = 0;
    rx_head = rx_tail = 0;
    rx_armed = false;
}

static void usb_serial_disconnect(void)
{
    active = false;
    rx_armed = false;
    tx_inflight = 0;
    tx_tail = tx_head;
}

bool usb_serial_connected(void)
{
    return active;
}

int usb_serial_read(void *buf, int maxlen)
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

int usb_serial_write(const void *data, int length)
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

/* Non-blocking; drops what does not fit (logf). */
void usb_serial_send(const unsigned char *data, int length)
{
    if (!active || length <= 0)
        return;
    size_t space = TX_RING - (tx_head - tx_tail);
    size_t n = MIN(space, (size_t)length);
    size_t off = tx_head % TX_RING;
    size_t first = MIN(n, TX_RING - off);
    memcpy(&tx_ring[off], data, first);
    memcpy(tx_ring, data + first, n - first);
    tx_head += n;
    tx_kick();
}

/* called by usb_core_transfer_complete() */
static void usb_serial_transfer_complete(int ep,int dir, int status, int length)
{
    (void)ep;

    switch (dir) {
        case USB_DIR_OUT: {
            unsigned long now = serial_now_us();
            stats.rx_busy_us += now - rx_mark;
            rx_mark = now;
            rx_armed = false;
            int done = rx_cur;
            size_t n = (status == 0 && length > 0) ? MIN((size_t)length, (size_t)RX_XFER) : 0;
            stats.rx_xfers++;
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

struct usb_class_driver usb_cdrv_serial = {
    .needs_exclusive_storage = false,
    .needs_cpu_boost = false,
    .config = 1,
    .ep_allocs_size = ARRAYLEN(ep_allocs),
    .ep_allocs = ep_allocs,
    .set_first_interface = usb_serial_set_first_interface,
    .get_config_descriptor = usb_serial_get_config_descriptor,
    .init_connection = usb_serial_init_connection,
    .init = usb_serial_init,
    .disconnect = usb_serial_disconnect,
    .transfer_complete = usb_serial_transfer_complete,
    .control_request = usb_serial_control_request,
};
