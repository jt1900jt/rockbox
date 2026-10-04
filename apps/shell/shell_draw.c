/* Screen layout. Geometry follows the design mockups, scaled to the 320x240 panel;
 * the mockups are drawn at ~2x, so type sizes are set by legibility (10 px floor)
 * rather than by scaling the artwork down. */
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "metadata.h"
#include "playlist.h"
#include "settings.h"
#include "timefuncs.h"
#include "power.h"
#include "powermgmt.h"
#include "string-extra.h"

#include "shell.h"
#include "shell_art.h"
#include "shell_gfx.h"

#define STATUS_H   22
#define LIST_Y     24
#define ROW_H      36
#define THUMB      28
#define SIDE       10
#define SCROLL_W   3
/* selection bar geometry; row content must stay inside it */
#define SEL_X      (SIDE - 4)
#define SEL_W      (LCD_WIDTH - SEL_X - SCROLL_W - 5)
#define ROW_RIGHT  (SEL_X + SEL_W - 8)

/* Now Playing geometry */
#define NP_ART     116
#define NP_ART_X   14
#define NP_ART_Y   30
#define NP_TEXT_X  (NP_ART_X + NP_ART + 14)
#define BAR_Y      158
#define BAR_H      3

static int rows_visible(void)
{
    return (LCD_HEIGHT - LIST_Y) / ROW_H;
}

int draw_rows_visible(void)
{
    return rows_visible();
}

/* ---- status bar ---- */

/* The status bar sits on top of the Now Playing wash, so repainting it alone cannot
 * just fill with the background colour. The strip is saved on a full redraw and
 * restored before each partial repaint. */
static uint16_t status_bg[LCD_WIDTH * STATUS_H];
static bool status_bg_valid;

void draw_status_save(void)
{
    gfx_save(status_bg, 0, 0, LCD_WIDTH, STATUS_H);
    status_bg_valid = true;
}

void draw_status_invalidate(void)
{
    status_bg_valid = false;
}

void draw_status(const char *title, bool can_go_back)
{
    char up[96], buf[16];
    if (status_bg_valid)
        gfx_blit(status_bg, 0, 0, LCD_WIDTH, STATUS_H);
    else
        gfx_fill(0, 0, LCD_WIDTH, STATUS_H, C_BG);
    int cy = gfx_baseline_in(F_CAPS, 0, STATUS_H);
    int mid = STATUS_H / 2;

    int left = SIDE;
    if (can_go_back) {
        gfx_icon_chevron_left(left, mid - 5, C_FG);
        left += 14;
    }
    if (shell_show_clock()) {
        struct tm *t = get_time();
        if (t) {
            int hour = t->tm_hour;
            if (global_settings.timeformat) { /* 12 hour */
                hour = hour % 12;
                if (hour == 0)
                    hour = 12;
            }
            snprintf(buf, sizeof buf, "%d:%02d", hour, t->tm_min);
            gfx_text(F_CAPS, left + 2, cy, buf, C_SUB);
        }
    }

    /* Battery sits at the right edge, with the percentage printed inside the cell. */
    int bx = LCD_WIDTH - SIDE - GFX_BATTERY_W;
    int right = bx;
    if (shell_show_battery_pct())
        gfx_icon_battery(bx, mid - GFX_BATTERY_H / 2, battery_level(), charger_inserted(), C_SUB);
    else
        right = LCD_WIDTH - SIDE;

    /* Volume, shown where the wheel actually controls it. The number is the step
     * index rescaled to 0-100, so a full sweep always reads 0 to 100. */
    if (shell_status_volume()) {
        snprintf(buf, sizeof buf, "%d", shell_volume_percent());
        right -= gfx_width(F_CAPS, buf) + 8;
        gfx_text(F_CAPS, right, cy, buf, C_FG);
        right -= 14;
        gfx_icon_speaker(right, mid - 5, C_SUB);
    }

    int st = audio_status();
    if (st & AUDIO_STATUS_PLAY) {
        right -= 14;
        if (st & AUDIO_STATUS_PAUSE)
            gfx_icon_pause_c(right + 4, mid, C_SUB);
        else
            gfx_icon_play_c(right + 4, mid, C_SUB);
    }

    /* Keep the title centred on the screen: reserve the same width on both sides,
     * taken from whichever side needs more, so enabling the battery readout does
     * not shove the title off-centre. */
    /* Centre the title on screen when the symmetric space allows it, so enabling the
     * battery readout does not shove it sideways; fall back to centring in the gap
     * rather than truncating when the sides are busy. */
    gfx_upper(up, sizeof up, title);
    int need = MAX(left, LCD_WIDTH - right);
    int symmetric = LCD_WIDTH - 2 * need - 10;
    int gap = right - left - 10;
    int tw = gfx_width(F_CAPS, up);
    if (tw <= symmetric)
        gfx_text_center(F_CAPS, LCD_WIDTH / 2, cy, symmetric, up, C_FG);
    else if (gap > 40)
        gfx_text_center(F_CAPS, (left + right) / 2, cy, gap, up, C_FG);
}

/* ---- list ---- */

static void draw_thumb(int x, int y, uint32_t art_id, uint16_t swatch)
{
    int w, h;
    const uint16_t *px = art_get(IPAP_THMB, art_id, &w, &h);
    if (px && w == THUMB && h == THUMB)
        gfx_blit_round(px, x, y, THUMB, THUMB, 3);
    else
        gfx_fill_round(x, y, THUMB, THUMB, 3, swatch ? swatch : C_PLACEHOLD);
}

static void draw_row(const struct view *v, int i, int y, bool selected)
{
    struct row r;
    view_row(v, i, &r);

    if (selected)
        gfx_fill_round(SEL_X, y + 1, SEL_W, ROW_H - 2, 4, C_SEL_BG);
    else
        gfx_fill(0, y, LCD_WIDTH - SCROLL_W - 2, ROW_H, C_BG);

    int x = SIDE;
    if (r.has_thumb) {
        draw_thumb(x, y + (ROW_H - THUMB) / 2, r.art_id, r.swatch);
        x += THUMB + 10;
    }

    uint16_t fg = selected ? C_SEL_FG : C_FG;
    uint16_t sub = selected ? C_SEL_SUB : C_SUB;
    int right = ROW_RIGHT;
    int trail_w = r.trail[0] ? gfx_width(F_CAPS, r.trail) + 10 : 0;
    int text_w = right - trail_w - x;

    enum gfx_face tf = selected ? F_ROW_SEL : F_ROW;
    if (r.sub && r.sub[0]) {
        /* Two lines centred as a block: the pair sits on the row's optical centre
         * rather than each line being placed by hand. */
        const int gap = 2;
        int block = gfx_ascent(tf) + gap + gfx_line_height(F_SUB);
        int top = y + (ROW_H - block) / 2;
        gfx_text_fit(tf, x, top + gfx_ascent(tf), text_w, r.title, fg);
        gfx_text_fit(F_SUB, x, top + gfx_ascent(tf) + gap + gfx_ascent(F_SUB), text_w, r.sub, sub);
    } else {
        gfx_text_fit(tf, x, gfx_baseline_in(tf, y, ROW_H), text_w, r.title, fg);
    }
    if (trail_w)
        gfx_text_right(F_CAPS, right, gfx_baseline_in(F_CAPS, y, ROW_H), r.trail, sub);
}

static void draw_scrollbar(int count, int top)
{
    int rows = rows_visible();
    int track_y = LIST_Y + 2, track_h = LCD_HEIGHT - LIST_Y - 6;
    int x = LCD_WIDTH - SCROLL_W - 2;
    gfx_fill(x, track_y, SCROLL_W, track_h, C_BG);
    if (count <= rows)
        return;
    int th = track_h * rows / count;
    if (th < 14)
        th = 14;
    int ty = track_y + (track_h - th) * top / (count - rows);
    gfx_fill_round(x, track_y, SCROLL_W, track_h, 1, C_TRACK);
    gfx_fill_round(x, ty, SCROLL_W, th, 1, C_SUB);
}

/* Queue the art just outside the viewport so scrolling finds it already resident. */
static void prefetch_around(const struct view *v, int top, int count)
{
    int rows = rows_visible();
    for (int k = -2; k < rows + 2; k++) {
        int i = top + k;
        if (i < 0 || i >= count || (k >= 0 && k < rows))
            continue;
        struct row r;
        view_row(v, i, &r);
        if (r.has_thumb)
            art_prefetch(IPAP_THMB, r.art_id);
    }
}

void draw_list(const struct view *v, bool full)
{
    int count = view_count(v);
    int rows = rows_visible();

    if (full) {
        gfx_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
        draw_status(view_title(v), true);
    }

    if (count == 0) {
        gfx_fill(0, LIST_Y, LCD_WIDTH, LCD_HEIGHT - LIST_Y, C_BG);
        gfx_text_center(F_BODY, LCD_WIDTH / 2, LCD_HEIGHT / 2, 280, "Nothing here yet", C_SUB);
        return;
    }

    for (int k = 0; k < rows && v->top + k < count; k++)
        draw_row(v, v->top + k, LIST_Y + k * ROW_H, v->top + k == v->sel);

    /* clear any space below the last row */
    int used = LIST_Y + MIN(rows, count - v->top) * ROW_H;
    if (used < LCD_HEIGHT)
        gfx_fill(0, used, LCD_WIDTH - SCROLL_W - 2, LCD_HEIGHT - used, C_BG);

    draw_scrollbar(count, v->top);
    prefetch_around(v, v->top, count);
}

/* Selection moved without scrolling: repaint just the two affected rows.
 * A full redraw costs ~25 ms; two rows cost ~8 ms. */
void draw_list_rows(const struct view *v, int old_sel, int new_sel)
{
    int count = view_count(v);
    for (int i = 0; i < 2; i++) {
        int idx = i ? new_sel : old_sel;
        if (idx < v->top || idx >= v->top + rows_visible() || idx >= count)
            continue;
        draw_row(v, idx, LIST_Y + (idx - v->top) * ROW_H, idx == new_sel);
    }
}

/* ---- header list (playlist / album detail) ---- */

void draw_list_header(const struct view *v, const char *title, const char *sub, uint32_t art_id, uint16_t swatch)
{
    const int h = 60;
    gfx_fill(0, STATUS_H, LCD_WIDTH, h, C_BG);
    int w, ih;
    const uint16_t *px = art_get(IPAP_HEAD, art_id, &w, &ih);
    if (px && w == 52 && ih == 52)
        gfx_blit_round(px, SIDE, STATUS_H + 4, 52, 52, 4);
    else
        gfx_fill_round(SIDE, STATUS_H + 4, 52, 52, 4, swatch ? swatch : C_PLACEHOLD);

    int x = SIDE + 52 + 12;
    int tw = LCD_WIDTH - x - SIDE;
    if (sub) {
        int block = gfx_ascent(F_TITLE) + 4 + gfx_line_height(F_CAPS);
        int top = STATUS_H + 4 + (52 - block) / 2;
        gfx_text_fit(F_TITLE, x, top + gfx_ascent(F_TITLE), tw, title, C_FG);
        char up[64];
        gfx_upper(up, sizeof up, sub);
        gfx_text_fit(F_CAPS, x, top + gfx_ascent(F_TITLE) + 4 + gfx_ascent(F_CAPS), tw, up, C_SUB);
    } else {
        gfx_text_fit(F_TITLE, x, gfx_baseline_in(F_TITLE, STATUS_H + 4, 52), tw, title, C_FG);
    }
    gfx_hline(SIDE, STATUS_H + h - 1, LCD_WIDTH - 2 * SIDE, C_HAIRLINE);
    (void)v;
}

/* ---- home ---- */

void draw_home(int sel, const char *const *labels, int n, uint32_t art_id, uint16_t swatch)
{
    gfx_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);

    /* Blurred album art behind the menu, darkened so the labels stay legible. */
    int bw, bh;
    const uint16_t *blur = art_get(IPAP_BLUR, art_id, &bw, &bh);
    if (blur) {
        gfx_blit_scaled(blur, bw, bh, 0, 0, LCD_WIDTH, LCD_HEIGHT);
        gfx_scrim(0, 0, LCD_WIDTH, LCD_HEIGHT, 150);
        /* extra darkening on the left, under the labels */
        gfx_scrim(0, 0, LCD_WIDTH / 2, LCD_HEIGHT, 70);
    } else if (swatch) {
        gfx_wash(0, 0, LCD_WIDTH, LCD_HEIGHT, LCD_WIDTH * 3 / 4, LCD_HEIGHT / 2, LCD_WIDTH, swatch, C_BG);
        gfx_scrim(0, 0, LCD_WIDTH, LCD_HEIGHT, 120);
    }

    draw_status("Music", false);

    /* Fit the menu between the status bar and the bottom edge; with every item shown
     * the natural spacing would run under the status bar. */
    const int top_pad = 6, bot_pad = 6;
    int avail = LCD_HEIGHT - STATUS_H - top_pad - bot_pad;
    int line = n > 0 ? avail / n : avail;
    if (line > 34)
        line = 34;
    int y0 = STATUS_H + top_pad + (avail - n * line) / 2;
    for (int i = 0; i < n; i++) {
        char up[32];
        gfx_upper(up, sizeof up, labels[i]);
        bool is_sel = i == sel;
        enum gfx_face f = is_sel ? F_TITLE : F_MENU_SEL;
        int y = gfx_baseline_in(f, y0 + i * line, line);
        if (is_sel)
            gfx_fill(SIDE + 2, y - gfx_ascent(f) / 2 - 1, 6, 6, C_FG);
        gfx_text(f, SIDE + 16, y, up, is_sel ? C_FG : C_DIM);
    }

    /* now-playing card on the right */
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    if (id3) {
        const int cx = 190, cy = 40, cw = 116;
        int w, h;
        const uint16_t *px = art_get(IPAP_LRGE, art_id, &w, &h);
        if (px && w >= cw && h >= cw) {
            /* centre-crop the large art to the card */
            static uint16_t tmp[116 * 116];
            int off = (w - cw) / 2;
            for (int r = 0; r < cw; r++)
                memcpy(&tmp[r * cw], &px[(r + off) * w + off], cw * 2);
            gfx_blit_round(tmp, cx, cy, cw, cw, 4);
        } else {
            gfx_fill_round(cx, cy, cw, cw, 4, swatch ? swatch : C_PLACEHOLD);
        }
        gfx_text_fit(F_ROW, cx, cy + cw + 18, cw, id3->title ? id3->title : "", C_FG);
        gfx_text_fit(F_SUB, cx, cy + cw + 34, cw, id3->artist ? id3->artist : "", C_SUB);
    }
}

/* ---- cover flow ---- */

/* Covers are drawn flat at three sizes rather than with a perspective transform: a real
 * 3D projection needs per-pixel sampling of every side cover, and a full-screen push is
 * already 25 ms on this hardware. Scaling plus dimming reads as depth at this size.
 */
#define CF_CENTER   116
#define CF_SIDE     74
#define CF_FAR      52
#define CF_ART_Y    40

static void cf_cover(const uint16_t *src, int sw, int x, int y, int size, uint8_t dim)
{
    if (!src) {
        gfx_fill_round(x, y, size, size, 3, C_PLACEHOLD);
        return;
    }
    if (size == sw)
        gfx_blit_round(src, x, y, size, size, 3);
    else
        gfx_blit_scaled(src, sw, sw, x, y, size, size);
    if (dim)
        gfx_scrim(x, y, size, size, dim);
}

/* Art for an album, if resident; queues a load otherwise. */
static const uint16_t *cf_art(const struct view *v, int index, int *w)
{
    int count = view_count(v);
    if (index < 0 || index >= count)
        return NULL;
    const ipdb_album *al = ipdb_album_at(&shell_db, (uint32_t)index);
    int h;
    const uint16_t *px = art_get(IPAP_LRGE, al->art_id, w, &h);
    return (px && *w == h) ? px : NULL;
}

void draw_coverflow(const struct view *v)
{
    int count = view_count(v);
    gfx_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);

    if (count == 0) {
        draw_status("Albums", true);
        gfx_text_center(F_BODY, LCD_WIDTH / 2, LCD_HEIGHT / 2, 280, "No albums", C_SUB);
        return;
    }

    const ipdb_album *cur = ipdb_album_at(&shell_db, (uint32_t)v->sel);
    /* Wash in the current album's colour, so the background follows the selection. */
    if (cur->colors[0]) {
        gfx_wash(0, 0, LCD_WIDTH, LCD_HEIGHT, LCD_WIDTH / 2, CF_ART_Y + CF_CENTER / 2,
                 220, cur->colors[0], C_BG);
        gfx_scrim(0, 0, LCD_WIDTH, LCD_HEIGHT, 130);
    }
    draw_status("Albums", true);

    const int cx = LCD_WIDTH / 2;
    int w;
    /* Far, then side, then centre: nearer covers overlap the ones behind them. */
    const uint16_t *px = cf_art(v, v->sel - 2, &w);
    cf_cover(px, w, cx - CF_CENTER / 2 - CF_SIDE - 28, CF_ART_Y + 40, CF_FAR, 150);
    px = cf_art(v, v->sel + 2, &w);
    cf_cover(px, w, cx + CF_CENTER / 2 + CF_SIDE + 28 - CF_FAR, CF_ART_Y + 40, CF_FAR, 150);

    px = cf_art(v, v->sel - 1, &w);
    cf_cover(px, w, cx - CF_CENTER / 2 - CF_SIDE + 12, CF_ART_Y + 22, CF_SIDE, 90);
    px = cf_art(v, v->sel + 1, &w);
    cf_cover(px, w, cx + CF_CENTER / 2 - 12, CF_ART_Y + 22, CF_SIDE, 90);

    px = cf_art(v, v->sel, &w);
    cf_cover(px, w, cx - CF_CENTER / 2, CF_ART_Y, CF_CENTER, 0);

    /* Title and artist below the stack. */
    int ty = CF_ART_Y + CF_CENTER + 18;
    gfx_text_center(F_TITLE, cx, ty, LCD_WIDTH - 2 * SIDE, ipdb_str(&shell_db, cur->title), C_FG);
    const ipdb_group *ar = &shell_db.artists[cur->artist_id];
    gfx_text_center(F_BODY, cx, ty + 20, LCD_WIDTH - 2 * SIDE, ipdb_str(&shell_db, ar->name), C_SUB);

    char pos[24];
    snprintf(pos, sizeof pos, "%d of %d", v->sel + 1, count);
    gfx_text_center(F_CAPS, cx, LCD_HEIGHT - 10, 200, pos, C_DIM);

    /* Pull in the covers either side so a turn of the wheel finds them loaded. */
    for (int d = -3; d <= 3; d++) {
        int i = v->sel + d;
        if (i >= 0 && i < count)
            art_prefetch(IPAP_LRGE, ipdb_album_at(&shell_db, (uint32_t)i)->art_id);
    }
}

/* ---- now playing ---- */

void format_duration(char *buf, int len, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600)
        snprintf(buf, len, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                 (unsigned long)(s % 60));
    else
        snprintf(buf, len, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static const char *codec_name(uint8_t c)
{
    static const char *const names[] = { "", "MP3", "AAC", "ALAC", "FLAC", "Vorbis", "Opus",
                                         "WAV", "AIFF", "WavPack", "APE", "Musepack" };
    return c < sizeof names / sizeof names[0] ? names[c] : "";
}

/* Five stars, filled to `rating`. Drawn as small diamonds: at this size a star outline
 * turns to mush, while a solid shape reads clearly filled or empty. */
#define STAR_W 13
static void draw_stars(uint8_t rating, int x, int y)
{
    for (int i = 0; i < 5; i++) {
        uint16_t c = i < rating ? C_FG : C_TRACK;
        int cx = x + i * STAR_W;
        for (int r = 0; r < 5; r++) {
            int w = 1 + 2 * (r < 3 ? r : 4 - r);
            gfx_fill(cx + 4 - w / 2, y + r, w, 1, c);
        }
    }
}

void draw_rating_overlay(const struct np_info *np)
{
    gfx_fill(NP_TEXT_X, NP_ART_Y + 100, LCD_WIDTH - NP_TEXT_X - SIDE, 14, C_BG);
    draw_stars(np->rating, NP_TEXT_X, NP_ART_Y + 106);
}

/* The progress area sits on top of the colour wash, so the per-second tick cannot just
 * fill with the background colour: that leaves a black band across the gradient. Instead
 * the strip is saved once per full redraw and restored before each tick. */
#define PROG_X NP_ART_X
#define PROG_W (LCD_WIDTH - 2 * NP_ART_X)
#define PROG_H 20
static uint16_t prog_bg[PROG_W * PROG_H];
static bool prog_bg_valid;

/* While a seek is held the bar shows where it would land, not where playback is. */
static bool seek_preview;
static unsigned long seek_preview_ms;
static void draw_progress(const struct mp3entry *id3);

void draw_now_playing_seek(unsigned long target_ms)
{
    struct mp3entry *id3 = audio_current_track();
    if (!id3)
        return;
    seek_preview = true;
    seek_preview_ms = target_ms;
    draw_progress(id3);
    seek_preview = false;
}

static void draw_progress(const struct mp3entry *id3)
{
    char buf[32], buf2[16];
    unsigned long len = id3->length;
    unsigned long el = seek_preview ? seek_preview_ms : id3->elapsed;

    if (prog_bg_valid)
        gfx_blit(prog_bg, PROG_X, BAR_Y, PROG_W, PROG_H);

    gfx_fill(PROG_X, BAR_Y, PROG_W, BAR_H, C_TRACK);
    if (len)
        gfx_fill(PROG_X, BAR_Y, (int)((unsigned long long)PROG_W * (el > len ? len : el) / len), BAR_H, C_FG);

    int ty = BAR_Y + BAR_H + 5 + gfx_ascent(F_CAPS);
    format_duration(buf, sizeof buf, el);
    gfx_text(F_CAPS, PROG_X, ty, buf, C_SUB);
    format_duration(buf2, sizeof buf2, len > el ? len - el : 0);
    snprintf(buf, sizeof buf, "-%s", buf2);
    gfx_text_right(F_CAPS, LCD_WIDTH - PROG_X, ty, buf, C_SUB);
}

void draw_now_playing(const struct np_info *np, bool full)
{
    char buf[64];
    struct mp3entry *id3 = audio_current_track();
    if (!id3 || !(audio_status() & AUDIO_STATUS_PLAY)) {
        gfx_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
        draw_status("Now Playing", true);
        gfx_text_center(F_BODY, LCD_WIDTH / 2, LCD_HEIGHT / 2, 280, "Nothing playing", C_SUB);
        return;
    }

    if (!full && prog_bg_valid) {
        draw_progress(id3);
        return;
    }
    prog_bg_valid = false;

    /* Background: a wash in the album's dominant colour, as in the mockup. */
    gfx_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    if (np->colors[0]) {
        /* A wash centred on the art, plus a cooler accent in the far corner; both fade
         * to nothing, so they blend rather than ending on an edge. */
        gfx_wash(0, 0, LCD_WIDTH, LCD_HEIGHT, NP_ART_X + NP_ART / 2, NP_ART_Y + NP_ART / 2,
                 240, np->colors[0], C_BG);
        if (np->colors[1])
            gfx_wash(0, 0, LCD_WIDTH, LCD_HEIGHT, LCD_WIDTH + 20, LCD_HEIGHT + 10,
                     200, np->colors[1], C_BG);
        gfx_scrim(0, 0, LCD_WIDTH, LCD_HEIGHT, 110);
    }
    /* Capture the bare strip before anything is drawn into it: a partial repaint
     * restores this, so it must not already contain the previous reading. */
    draw_status_save();
    draw_status("Now Playing", true);

    int w, h;
    const uint16_t *px = art_get(IPAP_LRGE, np->art_id, &w, &h);
    if (px && w == NP_ART && h == NP_ART)
        gfx_blit_round(px, NP_ART_X, NP_ART_Y, NP_ART, NP_ART, 4);
    else
        gfx_fill_round(NP_ART_X, NP_ART_Y, NP_ART, NP_ART, 4, np->colors[0] ? np->colors[0] : C_PLACEHOLD);

    int tw = LCD_WIDTH - NP_TEXT_X - SIDE;
    gfx_text_fit(F_TITLE, NP_TEXT_X, NP_ART_Y + 22, tw, id3->title ? id3->title : "", C_FG);
    gfx_text_fit(F_ROW, NP_TEXT_X, NP_ART_Y + 44, tw, id3->artist ? id3->artist : "", C_FG);
    gfx_text_fit(F_BODY, NP_TEXT_X, NP_ART_Y + 63, tw, id3->album ? id3->album : "", C_SUB);

    snprintf(buf, sizeof buf, "%d of %d", playlist_get_display_index(), playlist_amount());
    gfx_text(F_CAPS, NP_TEXT_X, NP_ART_Y + 90, buf, C_SUB);
    draw_stars(np->rating, NP_TEXT_X, NP_ART_Y + 106);

    gfx_save(prog_bg, PROG_X, BAR_Y, PROG_W, PROG_H);
    prog_bg_valid = true;
    draw_progress(id3);

    /* transport: a filled circle with the current state, flanked by skip glyphs.
     * Kept clear of the progress strip, which is restored on every tick. */
    const int cy = 199, cx = LCD_WIDTH / 2, skip_gap = 46;
    gfx_fill_round(cx - 15, cy - 15, 30, 30, 15, C_FG);
    if (audio_status() & AUDIO_STATUS_PAUSE)
        gfx_icon_play_c(cx, cy, C_SEL_FG);
    else
        gfx_icon_pause_c(cx, cy, C_SEL_FG);
    gfx_icon_prev_c(cx - skip_gap, cy, C_SUB);
    gfx_icon_next_c(cx + skip_gap, cy, C_SUB);

    /* Mode indicators: S for shuffle, R (R1 for repeat-one) for repeat. Lit when on,
     * sharing the transport's centre line. */
    int my = gfx_baseline_in(F_ROW, cy - 10, 20);
    gfx_text(F_ROW, SIDE + 2, my, "S", global_settings.playlist_shuffle ? C_FG : C_OFF);
    bool rep = global_settings.repeat_mode != REPEAT_OFF;
    const char *rlabel = global_settings.repeat_mode == REPEAT_ONE ? "R1" : "R";
    gfx_text_right(F_ROW, LCD_WIDTH - SIDE - 2, my, rlabel, rep ? C_FG : C_OFF);

    int fy = gfx_baseline_in(F_CAPS, LCD_HEIGHT - 22, 20);
    if (np->codec) {
        if (np->lossless)
            snprintf(buf, sizeof buf, "%s \xc2\xb7 %u/%lu", codec_name(np->codec), np->bits,
                     (unsigned long)(np->sample_rate / 1000));
        else
            snprintf(buf, sizeof buf, "%s \xc2\xb7 %u KBPS", codec_name(np->codec), np->bitrate);
        char up[48];
        gfx_upper(up, sizeof up, buf);
        gfx_text_right(F_CAPS, LCD_WIDTH - SIDE, fy, up, C_SUB);
    }
}

/* ---- misc ---- */

/* Brief confirmation strip along the bottom, for actions with no visible result. */
void draw_toast(const char *text)
{
    const int h = 26;
    int y = LCD_HEIGHT - h - 6;
    gfx_fill_round(SIDE, y, LCD_WIDTH - 2 * SIDE, h, 6, C_SEL_BG);
    gfx_text_center(F_ROW, LCD_WIDTH / 2, gfx_baseline_in(F_ROW, y, h),
                    LCD_WIDTH - 4 * SIDE, text, C_SEL_FG);
}

void draw_letter_overlay(char letter)
{
    char s[2] = { letter, 0 };
    const int box = 56;
    int x = (LCD_WIDTH - box) / 2, y = (LCD_HEIGHT - box) / 2;
    gfx_fill_round(x, y, box, box, 10, C_SEL_BG);
    gfx_text_center(F_TITLE, LCD_WIDTH / 2, gfx_baseline_in(F_TITLE, y, box), box, s, C_SEL_FG);
}

void draw_message(const char *line1, const char *line2)
{
    gfx_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    gfx_text_center(F_TITLE, LCD_WIDTH / 2, LCD_HEIGHT / 2 - 6, 300, line1, C_FG);
    if (line2)
        gfx_text_center(F_SUB, LCD_WIDTH / 2, LCD_HEIGHT / 2 + 14, 300, line2, C_SUB);
}
