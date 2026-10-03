/*
 * Drawing primitives for the shell. Everything writes straight into Rockbox's RGB565
 * framebuffer; the caller decides which rectangles to push with gfx_flush().
 *
 * Full-screen pushes cost ~25 ms on the 7G (39 fps ceiling) and scale with pixel count,
 * so screens redraw only what changed.
 */
#ifndef SHELL_GFX_H
#define SHELL_GFX_H

#include <stdbool.h>
#include <stdint.h>

#include "ipfn.h"
#include "lcd.h"

#define SHELL_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* Palette, from the design mockups. */
#define C_BG        SHELL_RGB(11, 11, 12)
#define C_FG        SHELL_RGB(244, 244, 242)
#define C_SUB       SHELL_RGB(156, 156, 154)
#define C_DIM       SHELL_RGB(122, 122, 124)
#define C_SEL_BG    SHELL_RGB(244, 244, 242)
#define C_SEL_FG    SHELL_RGB(11, 11, 12)
#define C_SEL_SUB   SHELL_RGB(90, 90, 96)
#define C_TRACK     SHELL_RGB(48, 48, 52)
#define C_OFF       SHELL_RGB(96, 96, 100) /* inactive control: visible but clearly off */
#define C_PLACEHOLD SHELL_RGB(32, 32, 36)
#define C_HAIRLINE  SHELL_RGB(38, 38, 42)

typedef struct {
    int x, y, w, h;
} gfx_rect;

/* Fonts, indexed by face. */
enum gfx_face {
    F_TITLE,    /* Inter SemiBold 20 */
    F_MENU,     /* Inter SemiBold 15, tracked */
    F_MENU_SEL, /* Inter SemiBold 17, tracked: selected home item */
    F_ROW,      /* Inter SemiBold 14 */
    F_ROW_SEL,  /* Inter SemiBold 15: selected list row */
    F_BODY,     /* Inter Medium 13 */
    F_SUB,      /* Inter Regular 12 */
    F_CAPS,     /* Inter SemiBold 11, tracked */
    F_COUNT
};

/* Loads the font atlases. Returns false if any face is missing or invalid; the caller
 * should fall back to the Rockbox UI rather than draw with no fonts. */
bool gfx_init(void);
const ipfn_font *gfx_font(enum gfx_face face);

/* Dirty-rect tracking: mark what changed, then flush once per frame. */
void gfx_dirty(int x, int y, int w, int h);
void gfx_dirty_all(void);
void gfx_clip(int x, int y, int w, int h);
void gfx_clip_reset(void);
void gfx_flush(void);

void gfx_fill(int x, int y, int w, int h, uint16_t color);
void gfx_hline(int x, int y, int w, uint16_t color);
void gfx_vline(int x, int y, int h, uint16_t color);
/* Rounded rect, used for the selection bar and art placeholders. */
void gfx_fill_round(int x, int y, int w, int h, int r, uint16_t color);

/* Vertical gradient between two colors, dithered to hide RGB565 banding. */
void gfx_gradient_v(int x, int y, int w, int h, uint16_t top, uint16_t bottom);
/* Radial wash used behind Now Playing: `color` at the focus, fading to `bg`. */
void gfx_wash(int x, int y, int w, int h, int cx, int cy, int radius, uint16_t color, uint16_t bg);

/* Blit RGB565 pixels. `src` is w*h in row order. */
void gfx_blit(const uint16_t *src, int x, int y, int w, int h);
/* Copy a region out of the framebuffer, so it can be restored later. */
void gfx_save(uint16_t *dst, int x, int y, int w, int h);
/* Blit with the corners rounded off against the background behind them. */
void gfx_blit_round(const uint16_t *src, int x, int y, int w, int h, int r);
/* Upscale a small image (the 80x60 blurred background) over a region. */
void gfx_blit_scaled(const uint16_t *src, int sw, int sh, int x, int y, int w, int h);
/* Darken a region toward black; alpha 0-255. */
void gfx_scrim(int x, int y, int w, int h, uint8_t alpha);

/* Text. `x` is the pen start, `y` the baseline. Returns the advanced pen x. */
int gfx_text(enum gfx_face face, int x, int y, const char *s, uint16_t color);
/* Truncates with an ellipsis to fit max_w. */
int gfx_text_fit(enum gfx_face face, int x, int y, int max_w, const char *s, uint16_t color);
int gfx_text_right(enum gfx_face face, int right, int y, const char *s, uint16_t color);
int gfx_text_center(enum gfx_face face, int cx, int y, int max_w, const char *s, uint16_t color);
/* Uppercase ASCII copy, for the tracked caps labels in the design. */
void gfx_upper(char *dst, size_t len, const char *src);

int gfx_width(enum gfx_face face, const char *s);
int gfx_ascent(enum gfx_face face);
int gfx_line_height(enum gfx_face face);

/* Glyph-based icons drawn as paths, since the design's icons are not in the font. */
void gfx_icon_chevron_left(int x, int y, uint16_t color);
void gfx_icon_play(int x, int y, uint16_t color);
void gfx_icon_pause(int x, int y, uint16_t color);
void gfx_icon_prev(int x, int y, uint16_t color);
void gfx_icon_next(int x, int y, uint16_t color);
/* Battery with the percentage printed inside the cell. */
void gfx_icon_battery(int x, int y, int percent, bool charging, uint16_t color);
#define GFX_BATTERY_W 27
#define GFX_BATTERY_H 14
void gfx_icon_headphones(int x, int y, uint16_t color);
void gfx_icon_speaker(int x, int y, uint16_t color);
void gfx_icon_shuffle(int x, int y, uint16_t color);
void gfx_icon_repeat(int x, int y, bool one, uint16_t color);

#endif
