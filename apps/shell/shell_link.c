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

#ifdef USB_ENABLE_SERIAL
#include "link_proto.h"

static struct link link;
static uint8_t rxbuf[65536];

static int serial_write(void *ctx, const void *buf, size_t n)
{
    (void)ctx;
    return usb_serial_write(buf, (int)n) < 0 ? -1 : 0;
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
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_background(LCD_RGBPACK(11, 11, 12));
    lcd_set_foreground(LCD_RGBPACK(244, 244, 242));
    lcd_clear_display();
    draw_status("USB Link", true);
    lcd_setfont(FONT_SYSFIXED);
    lcd_set_drawmode(DRMODE_FG);
    line(34, "host:     %s", usb_serial_connected() ? "connected" : "waiting (open the companion)");
    line(54, "received: %lu KB  (%lu KB/s)", (unsigned long)(link.st.rx_bytes / 1024), rx_rate);
    line(68, "sent:     %lu KB  (%lu KB/s)", (unsigned long)(link.st.tx_bytes / 1024), tx_rate);
    line(88, "frames %lu  crc errors %lu", (unsigned long)link.st.frames, (unsigned long)link.st.crc_errors);
    line(102, "resyncs %lu  errors %lu", (unsigned long)link.st.resyncs, (unsigned long)link.st.errors);
    struct usb_serial_stats us;
    usb_serial_get_stats(&us);
    unsigned long tot = us.rx_busy_us + us.rx_idle_us;
    line(122, "rx %lu KB xfers, endpoint armed %lu%%", us.rx_xfer_size / 1024,
         tot ? (unsigned long)((unsigned long long)us.rx_busy_us * 100 / tot) : 0);
    line(136, "rx xfers %lu  stalls %lu  errors %lu", us.rx_xfers, us.rx_stalls, us.rx_errors);
    line(216, "MENU to leave");
    lcd_update();
}

void shell_link_run(void)
{
    struct link_io io = { .write = serial_write, .ctx = NULL, .hello = MODEL_NAME };
    link_init(&link, &io);
    long next_draw = 0, last_tick = current_tick;
    uint64_t last_rx = 0, last_tx = 0;
    unsigned long rx_rate = 0, tx_rate = 0;

    button_clear_queue();
    for (;;) {
        int n = usb_serial_read(rxbuf, sizeof rxbuf);
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
        if (b == BUTTON_MENU)
            return;
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
}

#else /* !USB_ENABLE_SERIAL */

void shell_link_run(void) {}
bool shell_link_available(void) { return false; }
void shell_link_setup(void) {}

#endif
