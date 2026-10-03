/* USB LINK screen: runs the link protocol over the CDC-ACM data interface and shows
 * live throughput. The iPod stays in the shell while connected (USB charge mode); hold
 * any button while plugging in to get disk mode instead. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "button.h"
#include "font.h"
#include "kernel.h"
#include "lcd.h"
#include "misc.h"
#include "system.h"
#include "usb.h"

#include "shell.h"
#include "shell_sync.h"

#if defined(USB_ENABLE_SERIAL) || defined(USB_ENABLE_BULK)
#include "link_proto.h"

static struct link link;
static uint8_t rxbuf[65536];

/* The link runs over whichever transport the host opened. The vendor bulk interface
 * (WebUSB) is preferred: CDC-ACM goes through the host tty layer, which fragments bulk
 * writes into ~150-byte transfers and caps host-to-device throughput below 1 MB/s. */
static bool use_bulk(void)
{
#ifdef USB_ENABLE_BULK
    return usb_bulk_connected();
#else
    return false;
#endif
}

static int link_write(void *ctx, const void *buf, size_t n)
{
    (void)ctx;
#ifdef USB_ENABLE_BULK
    if (use_bulk())
        return usb_bulk_write(buf, (int)n) < 0 ? -1 : 0;
#endif
#ifdef USB_ENABLE_SERIAL
    return usb_serial_write(buf, (int)n) < 0 ? -1 : 0;
#else
    return -1;
#endif
}

static int link_read(void *buf, int maxlen)
{
#ifdef USB_ENABLE_BULK
    if (use_bulk())
        return usb_bulk_read(buf, maxlen);
#endif
#ifdef USB_ENABLE_SERIAL
    return usb_serial_read(buf, maxlen);
#else
    (void)buf; (void)maxlen;
    return 0;
#endif
}

static void line(int y, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    lcd_putsxy(12, y, (const unsigned char *)buf);
}

static void draw(unsigned long rx_rate, unsigned long tx_rate)
{
    bool bulk = use_bulk();
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_background(LCD_RGBPACK(11, 11, 12));
    lcd_set_foreground(LCD_RGBPACK(244, 244, 242));
    lcd_clear_display();
    draw_status("USB Link", true);
    lcd_setfont(FONT_SYSFIXED);
    lcd_set_drawmode(DRMODE_FG);
    line(34, "host:     %s", bulk ? "connected (WebUSB bulk)"
                             : usb_serial_connected() ? "connected (serial)"
                                                      : "waiting (open the companion)");
    line(54, "received: %lu KB  (%lu KB/s)", (unsigned long)(link.st.rx_bytes / 1024), rx_rate);
    line(68, "sent:     %lu KB  (%lu KB/s)", (unsigned long)(link.st.tx_bytes / 1024), tx_rate);
    line(88, "frames %lu  crc errors %lu", (unsigned long)link.st.frames, (unsigned long)link.st.crc_errors);
    line(102, "resyncs %lu  errors %lu", (unsigned long)link.st.resyncs, (unsigned long)link.st.errors);
    unsigned long xfers = 0, bytes = 0, busy = 0, idle = 0, stalls = 0, errors = 0, xsize = 0;
#ifdef USB_ENABLE_BULK
    if (bulk) {
        struct usb_bulk_stats b;
        usb_bulk_get_stats(&b);
        xfers = b.rx_xfers; bytes = b.rx_bytes; busy = b.rx_busy_us; idle = b.rx_idle_us;
        stalls = b.rx_stalls; errors = b.rx_errors; xsize = b.rx_xfer_size;
    }
#endif
#ifdef USB_ENABLE_SERIAL
    if (!bulk) {
        struct usb_serial_stats u;
        usb_serial_get_stats(&u);
        xfers = u.rx_xfers; busy = u.rx_busy_us; idle = u.rx_idle_us;
        stalls = u.rx_stalls; errors = u.rx_errors; xsize = u.rx_xfer_size;
        bytes = (unsigned long)link.st.rx_bytes;
    }
#endif
    unsigned long tot = busy + idle;
    line(122, "rx %lu KB xfers, endpoint armed %lu%%", xsize / 1024,
         tot ? (unsigned long)((unsigned long long)busy * 100 / tot) : 0);
    line(136, "rx xfers %lu  avg %lu B", xfers, xfers ? bytes / xfers : 0);
    line(150, "rx stalls %lu  errors %lu", stalls, errors);
    line(216, "MENU to leave");
    lcd_update();
}

void shell_link_run(void)
{
    struct link_io io = { .write = link_write, .ctx = NULL, .hello = MODEL_NAME,
                          .fs = shell_sync_fs() };
    link_init(&link, &io);
    long next_draw = 0, last_tick = current_tick;
    uint64_t last_rx = 0, last_tx = 0;
    unsigned long rx_rate = 0, tx_rate = 0;

    button_clear_queue();
    for (;;) {
        int n = link_read(rxbuf, sizeof rxbuf);
        if (n > 0)
            link_feed(&link, rxbuf, (size_t)n);

        if (TIME_AFTER(current_tick, next_draw)) {
            long dt = current_tick - last_tick;
            if (dt >= HZ) {
                rx_rate = (unsigned long)((link.st.rx_bytes - last_rx) * HZ / 1024 / dt);
                tx_rate = (unsigned long)((link.st.tx_bytes - last_tx) * HZ / 1024 / dt);
                last_rx = link.st.rx_bytes;
                last_tx = link.st.tx_bytes;
                last_tick = current_tick;
            }
            draw(rx_rate, tx_rate);
            next_draw = current_tick + HZ / 4;
        }

        long b = button_get(false);
        if (b == BUTTON_MENU) {
            shell_sync_abort();
            return;
        }
        if (b != BUTTON_NONE && !(b & (BUTTON_REL | BUTTON_REPEAT)) && (b & SYS_EVENT))
            default_event_handler(b);
        if (n <= 0)
            yield();
    }
}

bool shell_link_available(void)
{
    return usb_inserted();
}

void shell_link_setup(void)
{
    /* Stay in the shell when plugged in; holding a button while connecting inverts
     * this and gives disk mode. */
#ifdef HAVE_USB_POWER
    usb_set_mode(USB_MODE_CHARGE);
#endif
    usb_set_serial(true);
#ifdef USB_ENABLE_BULK
    usb_set_bulk(true);
#endif
}

#else /* no link transport */

void shell_link_run(void) {}
bool shell_link_available(void) { return false; }
void shell_link_setup(void) {}

#endif
