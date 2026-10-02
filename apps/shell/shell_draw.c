/* Phase 1 renderer: Rockbox lcd_* primitives and bitmap fonts. Layout and colors follow
 * the design mockups; phase 3 replaces this with the compositor, AA fonts and real art. */
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "font.h"
#include "lcd.h"
#include "playlist.h"
#include "powermgmt.h"
#include "rbpaths.h"
#include "metadata.h"
#include "string-extra.h"

#include "shell.h"

#define STATUS_H   20
#define LIST_Y     22
#define ROW_H      36
#define ROWS       SHELL_LIST_ROWS
#define THUMB      28

#define C_BG       LCD_RGBPACK(11, 11, 12)
#define C_FG       LCD_RGBPACK(244, 244, 242)
#define C_SUB      LCD_RGBPACK(156, 156, 154)
#define C_DIM      LCD_RGBPACK(139, 139, 138)
#define C_SEL_SUB  LCD_RGBPACK(74, 74, 80)
#define C_TRACK    LCD_RGBPACK(48, 48, 50)
#define C_THUMB    LCD_RGBPACK(40, 40, 44)

static int f_title, f_body, f_small, f_small_bold;

static int load_font(const char *name)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, FONT_DIR "/%s", name);
    int id = font_load(path);
    return id >= 0 ? id : FONT_UI;
}

void draw_init(void)
{
    f_title = load_font("12-Adobe-Helvetica-Bold.fnt");
    f_body = load_font("12-Adobe-Helvetica.fnt");
    f_small = load_font("10-Adobe-Helvetica.fnt");
    f_small_bold = load_font("10-Adobe-Helvetica-Bold.fnt");
    lcd_set_viewport(NULL);
    lcd_set_backdrop(NULL);
}

void format_duration(char *buf, int len, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600)
        snprintf(buf, len, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                 (unsigned long)(s % 60));
    else
        snprintf(buf, len, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static int font_h(int font)
{
    return font_get(font)->height;
}

static int text_w(int font, const char *s)
{
    int w, h;
    lcd_setfont(font);
    lcd_getstringsize((const unsigned char *)s, &w, &h);
    return w;
}

/* Truncate at a UTF-8 boundary and append an ellipsis until the text fits. */
static const char *fit(int font, const char *s, int maxw, char *buf, size_t len)
{
    if (maxw <= 0 || text_w(font, s) <= maxw)
        return s;
    size_t n = strlen(s);
    if (n > len - 4)
        n = len - 4;
    while (n > 0) {
        do {
            n--;
        } while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80);
        memcpy(buf, s, n);
        memcpy(buf + n, "\xe2\x80\xa6", 4);
        if (text_w(font, buf) <= maxw)
            return buf;
    }
    return "\xe2\x80\xa6";
}

static void text(int font, int x, int y, int maxw, unsigned color, const char *s)
{
    char buf[256];
    const char *t = fit(font, s, maxw, buf, sizeof buf);
    lcd_setfont(font);
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(color);
    lcd_putsxy(x, y, (const unsigned char *)t);
}

static void text_right(int font, int right, int y, unsigned color, const char *s)
{
    text(font, right - text_w(font, s), y, 0, color, s);
}

static void text_center(int font, int cx, int y, int maxw, unsigned color, const char *s)
{
    char buf[256];
    const char *t = fit(font, s, maxw, buf, sizeof buf);
    text(font, cx - text_w(font, t) / 2, y, 0, color, t);
}

static void fill(int x, int y, int w, int h, unsigned color)
{
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(color);
    lcd_fillrect(x, y, w, h);
}

static void upper_ascii(char *dst, size_t len, const char *src)
{
    size_t i = 0;
    for (; src[i] && i < len - 1; i++)
        dst[i] = (src[i] >= 'a' && src[i] <= 'z') ? (char)(src[i] - 32) : src[i];
    dst[i] = 0;
}

/* State glyph: pause bars while paused (status bar) or while playing (transport button). */
static void play_glyph(int x, int y, unsigned color, bool transport)
{
    int st = audio_status();
    bool bars = transport ? (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE)
                          : (st & AUDIO_STATUS_PAUSE) != 0;
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(color);
    if (bars) {
        lcd_fillrect(x, y, 2, 8);
        lcd_fillrect(x + 4, y, 2, 8);
    } else if (st & AUDIO_STATUS_PLAY) {
        for (int i = 0; i < 4; i++)
            lcd_vline(x + i * 2, y + i, y + 7 - i), lcd_vline(x + i * 2 + 1, y + i, y + 7 - i);
    }
}

void draw_status(const char *title, bool can_go_back)
{
    char up[128];
    fill(0, 0, LCD_WIDTH, STATUS_H, C_BG);
    if (can_go_back) {
        lcd_set_foreground(C_FG);
        for (int i = 0; i < 2; i++) {
            lcd_drawline(16 + i, 5, 12 + i, 10);
            lcd_drawline(12 + i, 10, 16 + i, 15);
        }
    }
    upper_ascii(up, sizeof up, title);
    text_center(f_small_bold, LCD_WIDTH / 2, (STATUS_H - font_h(f_small_bold)) / 2, 200, C_FG, up);

    /* battery */
    int lvl = battery_level();
    int bx = LCD_WIDTH - 28, by = 6;
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(C_SUB);
    lcd_drawrect(bx, by, 16, 9);
    lcd_fillrect(bx + 16, by + 3, 2, 3);
    if (lvl > 0)
        fill(bx + 2, by + 2, (12 * lvl + 99) / 100, 5, C_FG);
    play_glyph(bx - 14, by, C_FG, false);
}

static void thumb(int x, int y, uint16_t swatch)
{
    /* Phase 1: dominant album color stands in for the art. */
    fill(x, y, THUMB, THUMB, swatch ? swatch : C_THUMB);
}

void draw_list(const struct view *v)
{
    struct row r;
    int n = view_count(v);
    fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    draw_status(view_title(v), true);

    if (n == 0) {
        text_center(f_body, LCD_WIDTH / 2, LCD_HEIGHT / 2 - 6, 280, C_SUB, "Nothing here yet");
        lcd_update();
        return;
    }

    for (int k = 0; k < ROWS && v->top + k < n; k++) {
        int i = v->top + k;
        int y = LIST_Y + k * ROW_H;
        bool sel = i == v->sel;
        view_row(v, i, &r);
        if (sel)
            fill(6, y, LCD_WIDTH - 14, ROW_H, C_FG);
        int x = 12;
        if (r.has_thumb) {
            thumb(10, y + (ROW_H - THUMB) / 2, r.swatch);
            x = 10 + THUMB + 8;
        }
        int tw = r.trail[0] ? text_w(f_small, r.trail) + 8 : 0;
        int right = LCD_WIDTH - 16;
        unsigned fg = sel ? C_BG : C_FG, sub = sel ? C_SEL_SUB : C_SUB;
        if (r.sub && r.sub[0]) {
            text(f_title, x, y + 4, right - tw - x, fg, r.title);
            text(f_small, x, y + 20, right - tw - x, sub, r.sub);
        } else {
            text(f_title, x, y + (ROW_H - font_h(f_title)) / 2, right - tw - x, fg, r.title);
        }
        if (tw)
            text_right(f_small, right, y + (ROW_H - font_h(f_small)) / 2, sub, r.trail);
    }

    /* scrollbar */
    if (n > ROWS) {
        int track_h = LCD_HEIGHT - LIST_Y - 6;
        int th = track_h * ROWS / n;
        if (th < 12)
            th = 12;
        int ty = LIST_Y + 2 + (track_h - th) * v->top / (n - ROWS);
        fill(LCD_WIDTH - 5, LIST_Y + 2, 2, track_h, C_TRACK);
        fill(LCD_WIDTH - 5, ty, 2, th, C_SUB);
    }
    lcd_update();
}

static uint16_t now_playing_color(const struct mp3entry *id3, const ipdb_track **out);

void draw_home(int sel, const char *const *labels, int n)
{
    fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    draw_status("Music", false);

    for (int i = 0; i < n; i++) {
        int y = 32 + i * 24;
        unsigned c = i == sel ? C_FG : C_DIM;
        if (i == sel)
            fill(14, y + 6, 5, 5, C_FG);
        text(f_title, 26, y, 160, c, labels[i]);
    }

    /* right panel: what's playing, or library size */
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    if (id3) {
        const ipdb_track *t;
        uint16_t color = now_playing_color(id3, &t);
        fill(196, 36, 110, 110, color ? color : C_THUMB);
        text(f_body, 196, 154, 116, C_FG, id3->title ? id3->title : "");
        text(f_small, 196, 170, 116, C_SUB, id3->artist ? id3->artist : "");
    } else {
        char buf[48];
        snprintf(buf, sizeof buf, "%lu songs", (unsigned long)shell_db.n_tracks);
        text(f_small, 196, 154, 116, C_SUB, buf);
    }
    lcd_update();
}

/* Album color for the playing track, found by path once per track change. */
static uint16_t now_playing_color(const struct mp3entry *id3, const ipdb_track **out)
{
    static char last_path[MAX_PATH];
    static uint16_t color;
    static const ipdb_track *track;
    if (strcmp(last_path, id3->path) != 0) {
        strlcpy(last_path, id3->path, sizeof last_path);
        color = 0;
        track = NULL;
        for (uint32_t i = 0; i < shell_db.n_tracks; i++) {
            const ipdb_track *t = &shell_db.tracks[i];
            if (!strcmp(ipdb_str(&shell_db, t->path), id3->path)) {
                const ipdb_album *al = &shell_db.albums[t->album_id];
                color = al->art_id != IPDB_NONE ? al->colors[0] : 0;
                track = t;
                break;
            }
        }
    }
    *out = track;
    return color;
}

static const char *codec_name(uint8_t c)
{
    static const char *const names[] = { "", "MP3", "AAC", "ALAC", "FLAC", "Vorbis", "Opus",
                                         "WAV", "AIFF", "WavPack", "APE", "Musepack" };
    return c < sizeof names / sizeof names[0] ? names[c] : "";
}

void draw_now_playing(void)
{
    char buf[64], buf2[16];
    fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    draw_status("Now Playing", true);

    struct mp3entry *id3 = audio_current_track();
    if (!id3 || !(audio_status() & AUDIO_STATUS_PLAY)) {
        text_center(f_body, LCD_WIDTH / 2, LCD_HEIGHT / 2 - 6, 280, C_SUB, "Nothing playing");
        lcd_update();
        return;
    }

    const ipdb_track *t;
    uint16_t color = now_playing_color(id3, &t);
    fill(14, 28, 116, 116, color ? color : C_THUMB);

    const char *title = id3->title ? id3->title : (t ? ipdb_str(&shell_db, t->title) : "");
    text(f_title, 142, 40, 166, C_FG, title);
    text(f_body, 142, 58, 166, C_FG, id3->artist ? id3->artist : "");
    text(f_small, 142, 74, 166, C_SUB, id3->album ? id3->album : "");
    snprintf(buf, sizeof buf, "%d of %d", playlist_get_display_index(), playlist_amount());
    text(f_small_bold, 142, 100, 166, C_SUB, buf);

    unsigned long len = id3->length, el = id3->elapsed;
    int bar = 292;
    fill(14, 158, bar, 3, C_TRACK);
    if (len)
        fill(14, 158, (int)((unsigned long long)bar * (el > len ? len : el) / len), 3, C_FG);
    format_duration(buf, sizeof buf, el);
    text(f_small, 14, 165, 0, C_SUB, buf);
    format_duration(buf2, sizeof buf2, len > el ? len - el : 0);
    snprintf(buf, sizeof buf, "-%s", buf2);
    text_right(f_small, 306, 165, C_SUB, buf);

    /* transport state */
    fill(LCD_WIDTH / 2 - 15, 184, 30, 30, C_FG);
    play_glyph(LCD_WIDTH / 2 - 3, 195, C_BG, true);

    if (t) {
        if (t->flags & IPDB_TF_LOSSLESS)
            snprintf(buf, sizeof buf, "%s \xc2\xb7 %u/%lu", codec_name(t->codec), t->bits,
                     (unsigned long)(t->sample_rate / 1000));
        else
            snprintf(buf, sizeof buf, "%s \xc2\xb7 %u kbps", codec_name(t->codec), t->bitrate_kbps);
        text_right(f_small_bold, 306, 222, C_SUB, buf);
    }
    lcd_update();
}

void draw_letter_overlay(char letter)
{
    char s[2] = { letter, 0 };
    int x = LCD_WIDTH / 2 - 28, y = LCD_HEIGHT / 2 - 28;
    fill(x, y, 56, 56, C_FG);
    text_center(f_title, LCD_WIDTH / 2, LCD_HEIGHT / 2 - font_h(f_title) / 2, 0, C_BG, s);
    lcd_update_rect(x, y, 56, 56);
}

void draw_message(const char *line1, const char *line2)
{
    fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    text_center(f_title, LCD_WIDTH / 2, LCD_HEIGHT / 2 - 16, 300, C_FG, line1);
    if (line2)
        text_center(f_small, LCD_WIDTH / 2, LCD_HEIGHT / 2 + 4, 300, C_SUB, line2);
    lcd_update();
}
