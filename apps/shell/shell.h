/*
 * iPod OS shell: replacement UI on top of Rockbox's firmware and playback core.
 * Reads the companion-built library from /.ipodos (see ipdb.h / docs/ipdb-format.md).
 */
#ifndef SHELL_H
#define SHELL_H

#include <stdbool.h>
#include <stdint.h>

#include "ipdb.h"

#define SHELL_DIR      "/.ipodos"
#define SHELL_DB_PATH  SHELL_DIR "/library.ipdb"

/* List geometry shared by navigation and drawing. */
#define SHELL_LIST_ROWS 6 /* must match draw_rows_visible() */

/* ---- views ---- */

enum view_kind {
    V_HOME,
    V_SONGS,
    V_ALBUMS,
    V_ARTISTS,
    V_GENRES,
    V_COMPOSERS,
    V_PLAYLISTS,
    V_ALBUM_TRACKS,        /* arg = album */
    V_ARTIST_ALBUMS,       /* arg = artist */
    V_ARTIST_ALBUM_TRACKS, /* arg = album, arg2 = artist (filtered) */
    V_GENRE_ARTISTS,       /* arg = genre */
    V_COMPOSER_TRACKS,     /* arg = composer */
    V_PLAYLIST_TRACKS,     /* arg = playlist */
    V_NOW_PLAYING,
    V_SETTINGS,
    V_COVERFLOW,
};

struct view {
    enum view_kind kind;
    uint32_t arg;
    uint32_t arg2;
    int sel;
    int top;
};

/* One list row, filled by view_row(). Strings point into the DB or into the row's buffers. */
struct row {
    const char *title;
    const char *sub;
    char subbuf[128];
    char trail[16];
    uint32_t art_id;   /* IPDB_NONE when the album has no cover */
    uint16_t swatch;   /* dominant album colour, used while the art loads */
    bool has_thumb;
};

/* What Now Playing needs from the library for the current track. */
struct np_info {
    uint32_t uid;
    uint8_t rating;
    uint32_t art_id;
    uint16_t colors[3];
    uint32_t sample_rate;
    uint16_t bitrate;
    uint8_t codec;
    uint8_t bits;
    bool lossless;
};

extern ipdb_db shell_db;

/* shell_views.c */
int view_count(const struct view *v);
void view_row(const struct view *v, int i, struct row *r);
const char *view_title(const struct view *v);
bool view_is_tracks(const struct view *v);
uint32_t view_track_id(const struct view *v, int i);
int view_jump_row(const struct view *v);  /* IPDB_JUMP_* or -1 */
bool view_child(const struct view *v, int i, struct view *child);
void view_prepare(struct view *v);        /* called once when a view is pushed */

/* shell_draw.c */
int draw_rows_visible(void);
void draw_status(const char *title, bool can_go_back);
/* Save the status strip for partial repaints; invalidate when the screen changes. */
void draw_status_save(void);
void draw_status_invalidate(void);
void draw_home(int sel, const char *const *labels, int n, uint32_t art_id, uint16_t swatch);
void draw_list(const struct view *v, bool full);
void draw_list_rows(const struct view *v, int old_sel, int new_sel);
void draw_coverflow(const struct view *v);
void draw_list_header(const struct view *v, const char *title, const char *sub,
                      uint32_t art_id, uint16_t swatch);
void draw_now_playing(const struct np_info *np, bool full);
/* Flash the rating stars after a change, without repainting the screen. */
void draw_rating_overlay(const struct np_info *np);
/* Repaints the progress bar showing where a held seek would land. */
void draw_now_playing_seek(unsigned long target_ms);
void draw_letter_overlay(char letter);
void draw_toast(const char *text);
void draw_message(const char *line1, const char *line2);
void format_duration(char *buf, int len, uint32_t ms);
/* Fills np from the library for the track currently playing. */
bool shell_np_info(struct np_info *np);

/* Settings list, defined in shell_main.c and rendered through the normal list views. */
int shell_settings_count(void);
bool shell_show_clock(void);
bool shell_show_battery_pct(void);
/* True while the wheel adjusts volume, i.e. on Now Playing. */
bool shell_status_volume(void);
int shell_volume_percent(void);
const char *shell_settings_label(int i);
const char *shell_settings_value(int i);

/* shell_bench.c */
extern long shell_boot_tick;
bool shell_bench_available(void);
void shell_bench_run(void);

/* shell_link.c */
void shell_link_setup(void);
bool shell_link_available(void);
void shell_link_run(void);

/* shell_input.c */
enum action {
    A_NONE,
    A_TIMEOUT,
    A_UP,
    A_DOWN,
    A_SELECT,
    A_BACK,
    A_HOME,
    A_PLAY,
    A_PREV,
    A_NEXT,
    A_SEEK_BACK,
    A_SEEK_FWD,
    A_SEEK_END, /* seek key released */
    A_RATE,     /* centre held: cycle the rating */
    A_USB,      /* USB session ended: library may have changed */
    A_QUIT,    /* simulator script finished */
};
void input_init(void);
enum action input_get(int timeout_ticks);
bool input_pending(void);
/* Rows (or steps) the last wheel event should move, from the driver's velocity.
 * Always at least 1. */
int input_wheel_steps(void);

#endif
