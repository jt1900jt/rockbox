#include "shell_gfx.h"

#include <stdio.h>
#include <string.h>

#include "core_alloc.h"
#include "file.h"
#include "lcd.h"
#include "system.h"

#include "shell.h"

#define FB(x, y) (*(uint16_t *)FBADDR((x), (y)))

struct face_spec {
    const char *file;
};

static const struct face_spec face_files[F_COUNT] = {
    [F_TITLE] = { "title-20" },
    [F_MENU] = { "menu-15" },
    [F_MENU_SEL] = { "menu-17" },
    [F_ROW] = { "row-14" },
    [F_ROW_SEL] = { "row-15" },
    [F_BODY] = { "body-13" },
    [F_SUB] = { "sub-12" },
    [F_CAPS] = { "caps-11" },
};

static ipfn_font fonts[F_COUNT];
static int font_handles[F_COUNT];

static gfx_rect dirty;
static bool dirty_any;

bool gfx_init(void)
{
    char path[MAX_PATH];
    for (int i = 0; i < F_COUNT; i++) {
        font_handles[i] = -1;
        snprintf(path, sizeof path, SHELL_DIR "/fonts/%s.ipfn", face_files[i].file);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            return false;
        off_t size = ffilesize(fd);
        if (size <= 0 || size > 1024 * 1024) {
            close(fd);
            return false;
        }
        int h = core_alloc((size_t)size);
        if (h <= 0) {
            close(fd);
            return false;
        }
        core_pin(h);
        void *buf = core_get_data(h);
        bool ok = read(fd, buf, (size_t)size) == size;
        close(fd);
        if (!ok || ipfn_open(&fonts[i], buf, (size_t)size) != IPFN_OK) {
            core_free(h);
            return false;
        }
        font_handles[i] = h;
    }
    return true;
}

const ipfn_font *gfx_font(enum gfx_face face)
{
    return &fonts[face];
}

/* ---- dirty rects ---- */

void gfx_dirty(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0)
        return;

    if (!dirty_any) {
        dirty.x = x; dirty.y = y; dirty.w = w; dirty.h = h;
        dirty_any = true;
        return;
    }
    int x2 = MAX(dirty.x + dirty.w, x + w);
    int y2 = MAX(dirty.y + dirty.h, y + h);
    dirty.x = MIN(dirty.x, x);
    dirty.y = MIN(dirty.y, y);
    dirty.w = x2 - dirty.x;
    dirty.h = y2 - dirty.y;
}

void gfx_dirty_all(void)
{
    gfx_dirty(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

void gfx_flush(void)
{
    if (!dirty_any)
        return;
    lcd_update_rect(dirty.x, dirty.y, dirty.w, dirty.h);
    dirty_any = false;
}

/* ---- primitives ---- */

static inline bool clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > LCD_WIDTH) *w = LCD_WIDTH - *x;
    if (*y + *h > LCD_HEIGHT) *h = LCD_HEIGHT - *y;
    return *w > 0 && *h > 0;
}

void gfx_fill(int x, int y, int w, int h, uint16_t color)
{
    if (!clip(&x, &y, &w, &h))
        return;
    for (int row = 0; row < h; row++) {
        uint16_t *p = &FB(x, y + row);
        for (int col = 0; col < w; col++)
            p[col] = color;
    }
    gfx_dirty(x, y, w, h);
}

void gfx_hline(int x, int y, int w, uint16_t color)
{
    gfx_fill(x, y, w, 1, color);
}

void gfx_vline(int x, int y, int h, uint16_t color)
{
    gfx_fill(x, y, 1, h, color);
}

/* Per-row inset of a rounded corner, so corners look round rather than chamfered. */
static int corner_inset(int r, int row)
{
    if (row >= r)
        return 0;
    int dy = r - row;
    int inset = 0;
    while (inset < r) {
        int dx = r - inset;
        if (dx * dx + dy * dy <= r * r)
            break;
        inset++;
    }
    return inset;
}

void gfx_fill_round(int x, int y, int w, int h, int r, uint16_t color)
{
    if (r <= 0 || w < 2 * r || h < 2 * r) {
        gfx_fill(x, y, w, h, color);
        return;
    }
    for (int row = 0; row < h; row++) {
        int from_edge = MIN(row, h - 1 - row);
        int inset = corner_inset(r, from_edge);
        gfx_fill(x + inset, y + row, w - 2 * inset, 1, color);
    }
}

/* 4x4 ordered dither: dark gradients band badly in RGB565 without it. */
static const uint8_t bayer[4][4] = {
    { 0, 8, 2, 10 },
    { 12, 4, 14, 6 },
    { 3, 11, 1, 9 },
    { 15, 7, 13, 5 },
};

static inline uint16_t mix565_dither(uint16_t a, uint16_t b, int num, int den, int x, int y)
{
    int t = bayer[y & 3][x & 3];
    /* channel ranges differ, so scale the dither offset per channel */
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = (ar * (den - num) + br * num) * 16 + t;
    int g = (ag * (den - num) + bg * num) * 16 + t;
    int bl = (ab * (den - num) + bb * num) * 16 + t;
    r /= den * 16; g /= den * 16; bl /= den * 16;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (bl > 31) bl = 31;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* Blend fg over dst with dithered rounding; RGB565 alone bands visibly on dark washes. */
static inline uint16_t blend_dither(uint16_t dst, uint16_t fg, uint8_t a, int x, int y)
{
    int t = bayer[y & 3][x & 3];
    int dr = (dst >> 11) & 31, dg = (dst >> 5) & 63, db = dst & 31;
    int fr = (fg >> 11) & 31, fgc = (fg >> 5) & 63, fb = fg & 31;
    int r = (dr * (255 - a) + fr * a) * 16 + t;
    int g = (dg * (255 - a) + fgc * a) * 16 + t;
    int b = (db * (255 - a) + fb * a) * 16 + t;
    r /= 255 * 16; g /= 255 * 16; b /= 255 * 16;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void gfx_gradient_v(int x, int y, int w, int h, uint16_t top, uint16_t bottom)
{
    int x0 = x, y0 = y, w0 = w, h0 = h;
    if (!clip(&x0, &y0, &w0, &h0))
        return;
    for (int row = 0; row < h0; row++) {
        int num = (y0 - y) + row;
        uint16_t *p = &FB(x0, y0 + row);
        for (int col = 0; col < w0; col++)
            p[col] = mix565_dither(top, bottom, num, h > 1 ? h - 1 : 1, x0 + col, y0 + row);
    }
    gfx_dirty(x0, y0, w0, h0);
}

void gfx_wash(int x, int y, int w, int h, int cx, int cy, int radius, uint16_t color, uint16_t bg)
{
    (void)bg;
    int x0 = x, y0 = y, w0 = w, h0 = h;
    if (!clip(&x0, &y0, &w0, &h0) || radius <= 0)
        return;
    int r2 = radius * radius;
    for (int row = 0; row < h0; row++) {
        int dy = (y0 + row) - cy;
        uint16_t *p = &FB(x0, y0 + row);
        for (int col = 0; col < w0; col++) {
            int dx = (x0 + col) - cx;
            int d2 = dx * dx + dy * dy;
            if (d2 >= r2)
                continue; /* outside the falloff: leave the pixel alone */
            /* Quadratic falloff, so the wash fades out smoothly rather than ending
             * on a visible edge. Blending over the current pixel lets washes stack. */
            int a = 255 - (int)((int64_t)d2 * 255 / r2);
            a = a * a / 255;
            p[col] = blend_dither(p[col], color, (uint8_t)a, x0 + col, y0 + row);
        }
    }
    gfx_dirty(x0, y0, w0, h0);
}

void gfx_blit(const uint16_t *src, int x, int y, int w, int h)
{
    int x0 = x, y0 = y, w0 = w, h0 = h;
    if (!clip(&x0, &y0, &w0, &h0))
        return;
    for (int row = 0; row < h0; row++) {
        const uint16_t *s = src + ((y0 - y) + row) * w + (x0 - x);
        memcpy(&FB(x0, y0 + row), s, (size_t)w0 * 2);
    }
    gfx_dirty(x0, y0, w0, h0);
}

void gfx_save(uint16_t *dst, int x, int y, int w, int h)
{
    int x0 = x, y0 = y, w0 = w, h0 = h;
    if (!clip(&x0, &y0, &w0, &h0))
        return;
    for (int row = 0; row < h0; row++)
        memcpy(dst + ((y0 - y) + row) * w + (x0 - x), &FB(x0, y0 + row), (size_t)w0 * 2);
}

void gfx_blit_round(const uint16_t *src, int x, int y, int w, int h, int r)
{
    if (r <= 0) {
        gfx_blit(src, x, y, w, h);
        return;
    }
    for (int row = 0; row < h; row++) {
        int from_edge = MIN(row, h - 1 - row);
        int inset = corner_inset(r, from_edge);
        int rx = x + inset, ry = y + row, rw = w - 2 * inset, rh = 1;
        if (!clip(&rx, &ry, &rw, &rh))
            continue;
        const uint16_t *s = src + row * w + (rx - x);
        memcpy(&FB(rx, ry), s, (size_t)rw * 2);
        gfx_dirty(rx, ry, rw, 1);
    }
}

void gfx_blit_scaled(const uint16_t *src, int sw, int sh, int x, int y, int w, int h)
{
    int x0 = x, y0 = y, w0 = w, h0 = h;
    if (!clip(&x0, &y0, &w0, &h0) || sw <= 0 || sh <= 0 || w <= 0 || h <= 0)
        return;
    /* Nearest neighbour is enough: the source is already blurred, so there is no
     * high-frequency detail for interpolation to preserve. */
    for (int row = 0; row < h0; row++) {
        int sy = ((y0 - y) + row) * sh / h;
        if (sy >= sh) sy = sh - 1;
        const uint16_t *s = src + sy * sw;
        uint16_t *p = &FB(x0, y0 + row);
        for (int col = 0; col < w0; col++) {
            int sx = ((x0 - x) + col) * sw / w;
            if (sx >= sw) sx = sw - 1;
            p[col] = s[sx];
        }
    }
    gfx_dirty(x0, y0, w0, h0);
}

void gfx_scrim(int x, int y, int w, int h, uint8_t alpha)
{
    int x0 = x, y0 = y, w0 = w, h0 = h;
    if (!clip(&x0, &y0, &w0, &h0) || alpha == 0)
        return;
    int keep = 255 - alpha;
    for (int row = 0; row < h0; row++) {
        uint16_t *p = &FB(x0, y0 + row);
        for (int col = 0; col < w0; col++) {
            uint16_t c = p[col];
            int r = ((c >> 11) & 31) * keep / 255;
            int g = ((c >> 5) & 63) * keep / 255;
            int b = (c & 31) * keep / 255;
            p[col] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
    gfx_dirty(x0, y0, w0, h0);
}

/* ---- text ---- */

static inline uint16_t blend565(uint16_t dst, uint16_t fg, uint8_t a)
{
    if (a == 255)
        return fg;
    int dr = (dst >> 11) & 31, dg = (dst >> 5) & 63, db = dst & 31;
    int fr = (fg >> 11) & 31, fg_ = (fg >> 5) & 63, fb = fg & 31;
    int r = dr + ((fr - dr) * a) / 255;
    int g = dg + ((fg_ - dg) * a) / 255;
    int b = db + ((fb - db) * a) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static int draw_run(const ipfn_font *f, int x, int y, const char *s, size_t n, uint16_t color)
{
    const char *p = s;
    int min_x = x, max_x = x, min_y = y, max_y = y;
    while (*p && (size_t)(p - s) < n) {
        const char *before = p;
        uint32_t cp = ipfn_utf8_next(&p);
        if ((size_t)(p - s) > n) {
            p = before;
            break;
        }
        ipfn_glyph g;
        ipfn_get_glyph(f, cp, &g);
        if (g.bitmap) {
            int gx = x + g.left, gy = y - g.top;
            for (int row = 0; row < g.h; row++) {
                int py = gy + row;
                if (py < 0 || py >= LCD_HEIGHT)
                    continue;
                const uint8_t *cov = g.bitmap + row * g.w;
                for (int col = 0; col < g.w; col++) {
                    int px = gx + col;
                    if (px < 0 || px >= LCD_WIDTH || !cov[col])
                        continue;
                    uint16_t *d = &FB(px, py);
                    *d = blend565(*d, color, cov[col]);
                }
            }
            min_x = MIN(min_x, gx);
            max_x = MAX(max_x, gx + g.w);
            min_y = MIN(min_y, gy);
            max_y = MAX(max_y, gy + g.h);
        }
        x += g.advance + f->tracking;
    }
    if (max_x > min_x && max_y > min_y)
        gfx_dirty(min_x, min_y, max_x - min_x, max_y - min_y);
    return x;
}

int gfx_text(enum gfx_face face, int x, int y, const char *s, uint16_t color)
{
    return draw_run(&fonts[face], x, y, s, (size_t)-1, color);
}

int gfx_text_fit(enum gfx_face face, int x, int y, int max_w, const char *s, uint16_t color)
{
    const ipfn_font *f = &fonts[face];
    if (max_w <= 0)
        return x;
    if (ipfn_width(f, s) <= max_w)
        return draw_run(f, x, y, s, (size_t)-1, color);

    static const char ell[] = "\xe2\x80\xa6"; /* U+2026 */
    int ell_w = ipfn_width(f, ell);
    size_t n = ipfn_fit(f, s, max_w - ell_w);
    int end = draw_run(f, x, y, s, n, color);
    return draw_run(f, end + (n ? f->tracking : 0), y, ell, (size_t)-1, color);
}

int gfx_text_right(enum gfx_face face, int right, int y, const char *s, uint16_t color)
{
    return gfx_text(face, right - gfx_width(face, s), y, s, color);
}

int gfx_text_center(enum gfx_face face, int cx, int y, int max_w, const char *s, uint16_t color)
{
    int w = gfx_width(face, s);
    if (w > max_w)
        return gfx_text_fit(face, cx - max_w / 2, y, max_w, s, color);
    return gfx_text(face, cx - w / 2, y, s, color);
}

void gfx_upper(char *dst, size_t len, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < len; i++)
        dst[i] = (src[i] >= 'a' && src[i] <= 'z') ? (char)(src[i] - 32) : src[i];
    dst[i] = 0;
}

int gfx_width(enum gfx_face face, const char *s)
{
    return ipfn_width(&fonts[face], s);
}

int gfx_ascent(enum gfx_face face)
{
    return fonts[face].ascent;
}

int gfx_line_height(enum gfx_face face)
{
    return fonts[face].line_height;
}

/* ---- icons ---- */

void gfx_icon_chevron_left(int x, int y, uint16_t color)
{
    for (int i = 0; i < 5; i++) {
        gfx_fill(x + 4 - i, y + i, 2, 1, color);
        gfx_fill(x + 4 - i, y + 9 - i, 2, 1, color);
    }
}

void gfx_icon_play(int x, int y, uint16_t color)
{
    /* triangle: full height at the left, tapering to a point on the right */
    for (int i = 0; i < 5; i++)
        gfx_fill(x + i, y + i, 1, 10 - i * 2, color);
}

void gfx_icon_pause(int x, int y, uint16_t color)
{
    gfx_fill(x, y, 3, 10, color);
    gfx_fill(x + 5, y, 3, 10, color);
}

void gfx_icon_prev(int x, int y, uint16_t color)
{
    /* bar at the left, then a triangle whose apex points left: narrow at x+3,
     * widening to full height at x+7 */
    gfx_fill(x, y, 2, 10, color);
    for (int i = 0; i < 5; i++)
        gfx_fill(x + 3 + i, y + 4 - i, 1, i * 2 + 2, color);
}

void gfx_icon_next(int x, int y, uint16_t color)
{
    /* triangle whose apex points right: full height at x, narrowing to x+4,
     * then the bar */
    for (int i = 0; i < 5; i++)
        gfx_fill(x + i, y + i, 1, 10 - i * 2, color);
    gfx_fill(x + 6, y, 2, 10, color);
}

void gfx_icon_battery(int x, int y, int percent, bool charging, uint16_t color)
{
    const int w = 18, h = 9;
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
    gfx_fill(x + w, y + 3, 2, 3, color);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int fill = (w - 4) * percent / 100;
    if (fill > 0)
        gfx_fill(x + 2, y + 2, fill, h - 4, color);
    if (charging) {
        /* a small bolt over the fill */
        for (int i = 0; i < 3; i++) {
            gfx_fill(x + w / 2 - i / 2, y + 2 + i, 1, 1, C_BG);
            gfx_fill(x + w / 2 + 1 - i / 2, y + h - 3 - i, 1, 1, C_BG);
        }
    }
}

void gfx_icon_headphones(int x, int y, uint16_t color)
{
    for (int i = 0; i < 9; i++) {
        int dy = (i < 2 || i > 6) ? 1 : 0;
        gfx_fill(x + i, y + dy, 1, 1, color);
    }
    gfx_fill(x, y + 2, 2, 5, color);
    gfx_fill(x + 7, y + 2, 2, 5, color);
}
