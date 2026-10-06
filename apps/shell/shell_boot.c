/*
 * Boot animation.
 *
 * The shell takes about a second to come up, nearly all of it validating the library,
 * and that second was a black screen. This fills it with the wordmark fading up and a
 * progress line underneath.
 *
 * Frames repaint only the strip the animation occupies. A full-screen push is ~25 ms on
 * the 7G, which would make the animation itself a measurable part of boot time; the
 * strip is about a fifth of that. The work between frames is real loading, so the
 * animation is driven by progress rather than by a timer: it never delays boot, and on a
 * fast library it simply finishes early.
 */
#include <string.h>

#include "kernel.h"
#include "lcd.h"

#include "shell.h"
#include "shell_gfx.h"

#define LOGO_H      44
#define BAR_W       120
#define BAR_H       2
#define LOGO_Y      ((LCD_HEIGHT - LOGO_H) / 2 - 6)
#define BAR_Y       (LOGO_Y + LOGO_H + 14)
#define STRIP_Y     (LOGO_Y - 4)
#define STRIP_H     (BAR_Y + BAR_H + 8 - STRIP_Y)

static bool active;
static int shown_pct = -1;

/* Blends a colour toward the background, for the fade. `a` is 0-255. */
static uint16_t fade(uint16_t fg, int a)
{
    if (a <= 0)
        return C_BG;
    if (a >= 255)
        return fg;
    int r = ((fg >> 11) & 0x1F) * a / 255;
    int g = ((fg >> 5) & 0x3F) * a / 255;
    int b = (fg & 0x1F) * a / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

#ifdef SIMULATOR
#include <stdio.h>
#include <stdlib.h>
void shell_shot(const char *name);
/* Captures each frame when IPODOS_BOOT_SHOT is set, so the animation can be reviewed
 * headless; the script harness cannot reach it. */
static void boot_capture(void)
{
    static int n;
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("IPODOS_BOOT_SHOT") != NULL;
    if (!enabled)
        return;
    char name[32];
    snprintf(name, sizeof name, "boot%02d", n++);
    shell_shot(name);
}
#else
#define boot_capture() do { } while (0)
#endif

static void draw_frame(int alpha, int pct)
{
    gfx_fill(0, STRIP_Y, LCD_WIDTH, STRIP_H, C_BG);

    gfx_text_center(F_TITLE, LCD_WIDTH / 2, LOGO_Y + gfx_ascent(F_TITLE) + 8,
                    LCD_WIDTH - 40, "iPod OS", fade(C_FG, alpha));

    int bx = (LCD_WIDTH - BAR_W) / 2;
    gfx_fill(bx, BAR_Y, BAR_W, BAR_H, fade(C_TRACK, alpha));
    if (pct > 0)
        gfx_fill(bx, BAR_Y, BAR_W * (pct > 100 ? 100 : pct) / 100, BAR_H, fade(C_FG, alpha));

    gfx_flush();
    boot_capture();
}

void boot_begin(void)
{
    active = true;
    shown_pct = -1;

    /* The whole screen is cleared once, so later frames need only the strip. */
    gfx_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    gfx_flush();

    /* Fade the wordmark up. Six frames at a strip each is ~30 ms in total, which is
     * cheap enough to spend before the real work starts. */
    for (int i = 1; i <= 6; i++)
        draw_frame(i * 255 / 6, 0);
}

void boot_progress(int pct)
{
    if (!active || pct == shown_pct)
        return;
    /* Only redraw when the bar would visibly move. */
    if (shown_pct >= 0 && pct - shown_pct < 4 && pct < 100)
        return;
    shown_pct = pct;
    draw_frame(255, pct);
}

void boot_end(void)
{
    if (!active)
        return;
    boot_progress(100);
    /* Fade back out rather than cutting: the home screen paints over a black frame
     * either way, and the fade hides the moment the library finishes loading. */
    for (int i = 5; i >= 0; i--)
        draw_frame(i * 255 / 6, 100);
    active = false;
}
