/* Click wheel and button mapping. In the simulator, IPODOS_SCRIPT drives the UI
 * headlessly and writes screenshots, so UI changes can be checked without hardware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "button.h"
#include "kernel.h"
#include "misc.h"
#include "lcd.h"
#include "file.h"
#include "dir.h"

#include "shell.h"

#ifdef SIMULATOR
static const char *script;
static const char *script_pos;

/* 24-bit BMP of the current framebuffer to /shots/<name>.bmp (simdisk). */
static void shot(const char *name, size_t len)
{
    char path[96];
    unsigned char hdr[54] = { 'B', 'M' };
    const int w = LCD_WIDTH, h = LCD_HEIGHT, row = w * 3;
    uint32_t size = 54 + (uint32_t)row * h;
    mkdir("/shots");
    snprintf(path, sizeof path, "/shots/%.*s.bmp", (int)len, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    write(fd, hdr, sizeof hdr);
    lcd_set_viewport(NULL);
    static unsigned char line[LCD_WIDTH * 3];
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            fb_data px = *FBADDR(x, y);
            line[x * 3 + 0] = (unsigned char)FB_UNPACK_BLUE(px);
            line[x * 3 + 1] = (unsigned char)FB_UNPACK_GREEN(px);
            line[x * 3 + 2] = (unsigned char)FB_UNPACK_RED(px);
        }
        write(fd, line, row);
    }
    close(fd);
}

/* Tokens (space separated): u d s m M p l r = up, down, select, menu, menu-hold(home),
 * play, prev, next; wN = wait N ticks; shot:NAME; q = quit. */
static enum action script_next(void)
{
    for (;;) {
        while (*script_pos == ' ')
            script_pos++;
        if (!*script_pos)
            return A_QUIT;
        const char *tok = script_pos;
        while (*script_pos && *script_pos != ' ')
            script_pos++;
        size_t len = (size_t)(script_pos - tok);
        if (len > 5 && !strncmp(tok, "shot:", 5)) {
            shot(tok + 5, len - 5);
            continue;
        }
        if (tok[0] == 'w') {
            sleep(atoi(tok + 1));
            return A_TIMEOUT;
        }
        sleep(HZ / 50);
        switch (tok[0]) {
        case 'u': return A_UP;
        case 'd': return A_DOWN;
        case 's': return A_SELECT;
        case 'm': return A_BACK;
        case 'M': return A_HOME;
        case 'p': return A_PLAY;
        case 'l': return A_PREV;
        case 'r': return A_NEXT;
        case 'L': return A_SEEK_BACK;
        case 'R': return A_SEEK_FWD;
        case 'E': return A_SEEK_END;
        case 'q': return A_QUIT;
        default:  continue;
        }
    }
}
#endif

void input_init(void)
{
#ifdef SIMULATOR
    script = getenv("IPODOS_SCRIPT");
    script_pos = script;
#endif
}

/* True when more button events are already waiting. The shell drains them before
 * drawing: a repaint costs 11-26 ms, and with a codec running the wheel can outpace
 * that, so rendering every intermediate position makes the UI lag behind the wheel. */
bool input_pending(void)
{
#ifdef SIMULATOR
    if (script)
        return false;
#endif
    return button_queue_count() > 0;
}

enum action input_get(int timeout_ticks)
{
#ifdef SIMULATOR
    if (script)
        return script_next();
#endif
    long b = button_get_w_tmo(timeout_ticks);
    if (b == BUTTON_NONE)
        return A_TIMEOUT;

    switch (b) {
    case BUTTON_SCROLL_FWD:
    case BUTTON_SCROLL_FWD | BUTTON_REPEAT:
        return A_DOWN;
    case BUTTON_SCROLL_BACK:
    case BUTTON_SCROLL_BACK | BUTTON_REPEAT:
        return A_UP;
    case BUTTON_SELECT:
        return A_SELECT;
    case BUTTON_MENU:
        return A_BACK;
    case BUTTON_MENU | BUTTON_REPEAT:
        return A_HOME;
    case BUTTON_PLAY:
        return A_PLAY;
    case BUTTON_LEFT:
        return A_PREV;
    case BUTTON_RIGHT:
        return A_NEXT;
    /* Held: seek while the key is down, as on the stock firmware. */
    case BUTTON_LEFT | BUTTON_REPEAT:
        return A_SEEK_BACK;
    case BUTTON_RIGHT | BUTTON_REPEAT:
        return A_SEEK_FWD;
    case BUTTON_LEFT | BUTTON_REL:
    case BUTTON_RIGHT | BUTTON_REL:
        return A_SEEK_END;
    default:
        break;
    }
    if (b & (BUTTON_REL | BUTTON_REPEAT))
        return A_NONE;
    if (default_event_handler(b) == SYS_USB_CONNECTED)
        return A_USB;
    return A_NONE;
}
