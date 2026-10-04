/* Shell entry: loads the library, runs the navigation stack, starts playback.
 * Falls back to the stock Rockbox UI when no valid library is present.
 *
 * Redraw policy: a full-screen push costs ~25 ms on the 7G, so screens repaint only what
 * changed. Moving the selection repaints two rows; the Now Playing tick repaints the
 * progress area. Everything else sets a full-redraw flag. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "core_alloc.h"
#include "file.h"
#include "kernel.h"
#include "misc.h"
#include "playlist.h"
#include "root_menu.h"
#include "screen_access.h"
#include "settings.h"
#include "sound.h"
#include "backlight.h"
#include "dsp_misc.h"
#include "viewport.h"

#include "shell.h"
#include "shell_art.h"
#include "shell_gfx.h"
#include "shell_journal.h"
#include "shell_main.h"
#include "string-extra.h"

#define STACK_MAX   12
#define MAX_DB_SIZE (48u * 1024 * 1024)
#define LETTER_TICKS (HZ * 3 / 4)
#define STAMP_PATH  SHELL_DIR "/.verified"

ipdb_db shell_db;
static int db_handle = -1;

static struct view stack[STACK_MAX];
static int depth;
static long overlay_until;
static char overlay_letter;

/* Redraw state. */
static bool need_full = true;
static int drawn_sel = -1, drawn_top = -1;
static bool seeking;
static long seek_offset; /* ms accumulated while a seek key is held */
static long seek_step;

/* Where a seek in progress would land. */
static unsigned long seek_target(void)
{
    struct mp3entry *id3 = audio_current_track();
    if (!id3)
        return 0;
    long t = (long)id3->elapsed + seek_offset;
    if (t < 0)
        t = 0;
    if (t > (long)id3->length)
        t = (long)id3->length;
    return (unsigned long)t;
}
static bool volume_dirty;
static long queued_tick; /* shows the "queued" confirmation briefly */


static void mark_full(void)
{
    need_full = true;
    drawn_sel = drawn_top = -1;
    draw_status_invalidate();
}

/* ---- library ---- */

static void db_unload(void)
{
    if (db_handle > 0)
        core_free(db_handle);
    db_handle = -1;
    memset(&shell_db, 0, sizeof shell_db);
}

/* The CRC covers the whole file and dominates load time (589 ms for a 7 MB library on the
 * 7G, against 24 ms for the structural checks). A stamp file records what was last
 * verified, so an unchanged library skips the CRC on later boots. The structural checks,
 * which are what keep drawing bounds-safe, always run. */
struct stamp {
    uint64_t generation;
    uint32_t size;
    uint32_t crc;
};

static bool stamp_matches(const struct stamp *want)
{
    struct stamp got;
    int fd = open(STAMP_PATH, O_RDONLY);
    if (fd < 0)
        return false;
    bool ok = read(fd, &got, sizeof got) == (ssize_t)sizeof got;
    close(fd);
    return ok && got.generation == want->generation && got.size == want->size && got.crc == want->crc;
}

static void stamp_write(const struct stamp *s)
{
    int fd = open(STAMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    write(fd, s, sizeof *s);
    close(fd);
}

/* 0 on success, an IPDB_E_* code, or -1 for I/O and memory errors. */
static int db_load(void)
{
    db_unload();
    int fd = open(SHELL_DB_PATH, O_RDONLY);
    if (fd < 0)
        return -1;
    off_t size = ffilesize(fd);
    if (size <= 0 || (unsigned long)size > MAX_DB_SIZE) {
        close(fd);
        return -1;
    }
    int h = core_alloc((size_t)size);
    if (h <= 0) {
        close(fd);
        return -1;
    }
    core_pin(h);
    void *buf = core_get_data(h);
    ssize_t got = read(fd, buf, (size_t)size);
    close(fd);
    if (got != size) {
        core_free(h);
        return -1;
    }
    struct stamp st = {
        .generation = ipdb_peek_generation(buf, (size_t)size),
        .size = (uint32_t)size,
        .crc = ipdb_peek_crc(buf, (size_t)size),
    };
    bool skip_crc = st.generation != 0 && stamp_matches(&st);

    int err = ipdb_open_ex(&shell_db, buf, (size_t)size, skip_crc);
    if (err != IPDB_OK) {
        core_free(h);
        return err;
    }
    if (!skip_crc)
        stamp_write(&st);
    db_handle = h;
    return 0;
}

/* ---- navigation ---- */

static struct view *top(void)
{
    return &stack[depth - 1];
}

static void push(const struct view *v)
{
    if (depth >= STACK_MAX)
        return;
    stack[depth] = *v;
    view_prepare(&stack[depth]);
    depth++;
    art_cancel_pending();
    mark_full();
}

static void pop(void)
{
    if (depth > 1)
        depth--;
    art_cancel_pending();
    mark_full();
}

static void push_kind(enum view_kind kind)
{
    struct view v = { .kind = kind };
    push(&v);
}

static void reset_to_home(void)
{
    memset(stack, 0, sizeof stack);
    depth = 1;
    stack[0].kind = V_HOME;
    mark_full();
}

static bool playing(void)
{
    return (audio_status() & AUDIO_STATUS_PLAY) != 0;
}

/* ---- now playing lookup ---- */

/* The DB is sorted by title, not path, so finding the playing track is a linear scan.
 * It runs once per track change, not per frame. */
static uint32_t current_uid = IPDB_NONE;
static uint32_t last_elapsed, last_length;
static uint8_t current_rating;

bool shell_np_info(struct np_info *np)
{
    static char last_path[MAX_PATH];
    static struct np_info cached;
    static bool cached_ok;

    struct mp3entry *id3 = audio_current_track();
    if (!id3 || !id3->path[0]) {
        memset(np, 0, sizeof *np);
        np->art_id = IPDB_NONE;
        return false;
    }
    if (strcmp(last_path, id3->path) != 0) {
        strlcpy(last_path, id3->path, sizeof last_path);
        memset(&cached, 0, sizeof cached);
        cached.art_id = IPDB_NONE;
        cached_ok = false;
        for (uint32_t i = 0; i < shell_db.n_tracks; i++) {
            const ipdb_track *t = &shell_db.tracks[i];
            if (strcmp(ipdb_str(&shell_db, t->path), id3->path) != 0)
                continue;
            const ipdb_album *al = &shell_db.albums[t->album_id];
            cached.art_id = al->art_id;
            cached.colors[0] = al->colors[0];
            cached.colors[1] = al->colors[1];
            cached.colors[2] = al->colors[2];
            cached.sample_rate = t->sample_rate;
            cached.bitrate = t->bitrate_kbps;
            cached.codec = t->codec;
            cached.bits = t->bits;
            cached.lossless = (t->flags & IPDB_TF_LOSSLESS) != 0;
            cached.uid = t->uid;
            cached_ok = true;
            break;
        }
    }
    *np = cached;
    np->rating = current_rating;
    return cached_ok;
}

/* The library is read-only, so what happened on the device goes into the journal for
 * the companion to merge. Watch for the track changing rather than hooking playback,
 * which keeps this out of the audio path. */
/* Rockbox stores the resume point in global_status, but writes it when its own UI saves
 * settings, which the shell never triggers. Keep it current here instead, throttled so a
 * long listen does not mean constant writes to storage. */
static void resume_poll(void)
{
    static long next_save;
    struct mp3entry *id3 = audio_current_track();

    if (!id3 || !(audio_status() & AUDIO_STATUS_PLAY))
        return;
    if (TIME_BEFORE(current_tick, next_save))
        return;
    next_save = current_tick + HZ * 15;

    int index = -1;
    if (playlist_get_resume_info(&index) < 0 || index < 0)
        return;
    global_status.resume_index = index;
    global_status.resume_crc32 = playlist_get_filename_crc32(NULL, index);
    global_status.resume_elapsed = (uint32_t)id3->elapsed;
    global_status.resume_offset = (uint32_t)id3->offset;
    status_save(false);
}

static void journal_poll(void)
{
    struct mp3entry *id3 = audio_current_track();
    struct np_info np;
    bool have = shell_np_info(&np);
    uint32_t uid = have ? np.uid : IPDB_NONE;

    if (uid != current_uid) {
        if (current_uid != IPDB_NONE)
            journal_track_finished(current_uid, last_elapsed, last_length);
        current_uid = uid;
        last_elapsed = last_length = 0;
        seeking = false;
        seek_offset = 0;
        current_rating = 0; /* ratings are per-track and come back from the companion */
    }
    /* Track the furthest point reached, not the latest sample: by the time a track
     * change is noticed the engine has already reset elapsed for the new track, which
     * would make every completed track look like a skip. */
    if (id3) {
        uint32_t el = (uint32_t)id3->elapsed;
        if (el > last_elapsed)
            last_elapsed = el;
        if (id3->length)
            last_length = (uint32_t)id3->length;
    }
}

/* ---- home ---- */

struct home_item {
    const char *label;
    enum view_kind kind;
    bool rockbox;
    bool bench;
    bool link;
};

/* Now Playing leads the list while something is playing, so it is the first thing
 * under the cursor on returning to Home. */
static const struct home_item home_all[] = {
    { "Now Playing", V_NOW_PLAYING, false, false, false },
    { "Playlists", V_PLAYLISTS, false, false, false },
    { "Artists", V_ARTISTS, false, false, false },
    { "Albums", V_ALBUMS, false, false, false },
    { "Cover Flow", V_COVERFLOW, false, false, false },
    { "Songs", V_SONGS, false, false, false },
    { "Settings", V_SETTINGS, false, false, false },
};
#define HOME_ALL ((int)(sizeof home_all / sizeof home_all[0]))

static int home_items(const struct home_item **out)
{
    int n = 0;
    for (int i = 0; i < HOME_ALL; i++) {
        if (home_all[i].kind == V_NOW_PLAYING && !playing())
            continue;
        out[n++] = &home_all[i];
    }
    return n;
}

static void home_render(void)
{
    const struct home_item *items[HOME_ALL];
    const char *labels[HOME_ALL];
    int n = home_items(items);
    if (stack[0].sel >= n)
        stack[0].sel = n - 1;
    for (int i = 0; i < n; i++)
        labels[i] = items[i]->label;

    struct np_info np;
    bool have = shell_np_info(&np);
    draw_home(stack[0].sel, labels, n, have ? np.art_id : IPDB_NONE, have ? np.colors[0] : 0);
}

static void enter_rockbox_ui(void)
{
    art_shutdown();
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, true, NULL);
    root_menu(); /* does not return */
}

static void home_select(void)
{
    const struct home_item *items[HOME_ALL];
    int n = home_items(items);
    if (stack[0].sel < 0 || stack[0].sel >= n)
        return;
    const struct home_item *it = items[stack[0].sel];
    if (it->rockbox)
        enter_rockbox_ui();
    else
        push_kind(it->kind);
}

/* ---- settings ---- */

enum setting_id {
    SET_CLOCK,
    SET_CLOCK_FORMAT,
    SET_BATTERY_PCT,
    SET_SHUFFLE,
    SET_REPEAT,
    SET_REPLAYGAIN,
    SET_BACKLIGHT,
    SET_USB_LINK,
    SET_BENCHMARK,
    SET_ROCKBOX,
    SET_COUNT
};

static const char *const setting_labels[SET_COUNT] = {
    [SET_CLOCK] = "Clock in Status Bar",
    [SET_CLOCK_FORMAT] = "Clock Format",
    [SET_BATTERY_PCT] = "Battery Percentage",
    [SET_SHUFFLE] = "Shuffle",
    [SET_REPLAYGAIN] = "Volume Levelling",
    [SET_BACKLIGHT] = "Backlight",
    [SET_REPEAT] = "Repeat",
    [SET_USB_LINK] = "USB Link",
    [SET_BENCHMARK] = "Benchmark",
    [SET_ROCKBOX] = "Rockbox Menu",
};

/* Shell display options. Stored alongside the library so they survive a firmware
 * update, which replaces .rockbox wholesale. */
struct shell_prefs {
    uint32_t magic;
    uint8_t clock;
    uint8_t battery_pct;
    uint8_t reserved[2];
};
#define PREFS_MAGIC 0x50534F49 /* "IOSP" */
#define PREFS_PATH SHELL_DIR "/prefs.bin"

static struct shell_prefs prefs = { PREFS_MAGIC, 1, 1, { 0, 0 } };

static void prefs_load(void)
{
    struct shell_prefs got;
    int fd = open(PREFS_PATH, O_RDONLY);
    if (fd < 0)
        return;
    bool ok = read(fd, &got, sizeof got) == (ssize_t)sizeof got;
    close(fd);
    if (ok && got.magic == PREFS_MAGIC)
        prefs = got;
}

static void prefs_save(void)
{
    int fd = open(PREFS_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    write(fd, &prefs, sizeof prefs);
    close(fd);
}

bool shell_show_clock(void)
{
    return prefs.clock != 0;
}

bool shell_show_battery_pct(void)
{
    return prefs.battery_pct != 0;
}

bool shell_status_volume(void)
{
    return depth > 0 && stack[depth - 1].kind == V_NOW_PLAYING;
}

/* The hardware range is about 80 steps; report it as 0-100 so a full sweep of the
 * wheel reads the way people expect. */
int shell_volume_percent(void)
{
    int lo = sound_min(SOUND_VOLUME), hi = sound_max(SOUND_VOLUME);
    int step = sound_steps(SOUND_VOLUME);
    if (step <= 0)
        step = 1;
    int steps = (hi - lo) / step;
    if (steps <= 0)
        return 0;
    int v = global_status.volume;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return ((v - lo) / step * 100 + steps / 2) / steps;
}

/* Entries that depend on hardware state are hidden rather than shown disabled. */
static bool setting_visible(int id)
{
    if (id == SET_CLOCK_FORMAT)
        return prefs.clock != 0;
    if (id == SET_USB_LINK)
        return shell_link_available();
    if (id == SET_BENCHMARK)
        return shell_bench_available();
    return true;
}

static int setting_at(int row)
{
    for (int i = 0; i < SET_COUNT; i++) {
        if (!setting_visible(i))
            continue;
        if (row-- == 0)
            return i;
    }
    return -1;
}

int shell_settings_count(void)
{
    int n = 0;
    for (int i = 0; i < SET_COUNT; i++)
        if (setting_visible(i))
            n++;
    return n;
}

const char *shell_settings_label(int row)
{
    int id = setting_at(row);
    return id >= 0 ? setting_labels[id] : "";
}

const char *shell_settings_value(int row)
{
    static const char *const repeat_names[] = { "Off", "All", "One", "Shuffle", "A-B" };
    int id = setting_at(row);
    switch (id) {
    case SET_CLOCK:
        return prefs.clock ? "On" : "Off";
    case SET_CLOCK_FORMAT:
        return global_settings.timeformat ? "12 Hour" : "24 Hour";
    case SET_BATTERY_PCT:
        return prefs.battery_pct ? "On" : "Off";
    case SET_SHUFFLE:
        return global_settings.playlist_shuffle ? "On" : "Off";
    case SET_REPLAYGAIN: {
        static const char *const rg[] = { "Track", "Album", "Smart", "Off" };
        int t = global_settings.replaygain_settings.type;
        return (t >= 0 && t < 4) ? rg[t] : "Off";
    }
    case SET_BACKLIGHT: {
        static char buf[16];
        int t = global_settings.backlight_timeout;
        if (t <= 0)
            return t < 0 ? "Always On" : "Off";
        snprintf(buf, sizeof buf, "%d s", t);
        return buf;
    }
    case SET_REPEAT:
        return global_settings.repeat_mode < (int)(sizeof repeat_names / sizeof repeat_names[0])
                   ? repeat_names[global_settings.repeat_mode] : "Off";
    default:
        return NULL;
    }
}

static void settings_select(int row)
{
    switch (setting_at(row)) {
    case SET_CLOCK:
        prefs.clock = !prefs.clock;
        prefs_save();
        break;
    case SET_CLOCK_FORMAT:
        global_settings.timeformat = !global_settings.timeformat;
        settings_save();
        break;
    case SET_BATTERY_PCT:
        prefs.battery_pct = !prefs.battery_pct;
        prefs_save();
        break;
    case SET_SHUFFLE:
        global_settings.playlist_shuffle = !global_settings.playlist_shuffle;
        settings_save();
        break;
    case SET_REPLAYGAIN: {
        /* Track, Album, Smart (album unless shuffling), Off. */
        int t = (global_settings.replaygain_settings.type + 1) % 4;
        global_settings.replaygain_settings.type = t;
        dsp_replaygain_set_settings(&global_settings.replaygain_settings);
        settings_save();
        break;
    }
    case SET_BACKLIGHT: {
        /* A short cycle of the values people actually pick. */
        static const int steps[] = { 5, 10, 30, 60, -1 };
        int cur = global_settings.backlight_timeout, next = steps[0];
        for (int i = 0; i < (int)(sizeof steps / sizeof steps[0]); i++) {
            if (steps[i] == cur) {
                next = steps[(i + 1) % (sizeof steps / sizeof steps[0])];
                break;
            }
        }
        global_settings.backlight_timeout = next;
        backlight_set_timeout(next);
        settings_save();
        break;
    }
    case SET_REPEAT:
        global_settings.repeat_mode = (global_settings.repeat_mode + 1) % NUM_REPEAT_MODES;
        audio_flush_and_reload_tracks();
        settings_save();
        break;
    case SET_USB_LINK:
        shell_link_run();
        break;
    case SET_BENCHMARK:
        shell_bench_run();
        break;
    case SET_ROCKBOX:
        enter_rockbox_ui();
        break;
    default:
        break;
    }
    mark_full();
}

/* ---- lists ---- */

static void list_clamp(struct view *v)
{
    int rows = v->kind == V_COVERFLOW ? 1 : draw_rows_visible();
    int n = view_count(v);
    if (v->sel >= n)
        v->sel = n - 1;
    if (v->sel < 0)
        v->sel = 0;
    if (v->top > v->sel)
        v->top = v->sel;
    if (v->sel >= v->top + rows)
        v->top = v->sel - rows + 1;
    if (v->top < 0)
        v->top = 0;
}

static void list_move(struct view *v, int delta)
{
    v->sel += delta;
    list_clamp(v);
}

static int bucket_end(int row, int b, int n)
{
    return b + 1 < IPDB_JUMP_BUCKETS ? (int)ipdb_jump(&shell_db, row, b + 1) : n;
}

static void letter_jump(struct view *v, int dir)
{
    int row = view_jump_row(v);
    int n = view_count(v);
    if (row < 0 || n == 0)
        return;
    int cur = 0;
    for (int b = 0; b < IPDB_JUMP_BUCKETS; b++) {
        if (v->sel >= (int)ipdb_jump(&shell_db, row, b) && v->sel < bucket_end(row, b, n)) {
            cur = b;
            break;
        }
    }
    for (int b = cur + dir; b >= 0 && b < IPDB_JUMP_BUCKETS; b += dir) {
        int start = (int)ipdb_jump(&shell_db, row, b);
        if (start < bucket_end(row, b, n)) {
            v->sel = start;
            v->top = start;
            list_clamp(v);
            overlay_letter = b < 26 ? (char)('A' + b) : '#';
            overlay_until = current_tick + LETTER_TICKS;
            mark_full();
            return;
        }
    }
}

static void play_view(const struct view *v, int start)
{
    int n = view_count(v);
    if (n == 0)
        return;
    int max = playlist_get_current()->max_playlist_size;
    int first = 0, count = n;
    if (count > max) {
        count = max;
        first = start - max / 2;
        if (first < 0)
            first = 0;
        if (first + count > n)
            first = n - count;
    }
    if (playlist_create(NULL, NULL) < 0)
        return;
    for (int i = first; i < first + count; i++) {
        const ipdb_track *t = ipdb_track_at(&shell_db, view_track_id(v, i));
        playlist_insert_track(NULL, ipdb_str(&shell_db, t->path), PLAYLIST_INSERT_LAST, false, false);
    }
    playlist_sync(NULL);
    int idx = start - first;
    if (global_settings.playlist_shuffle)
        idx = playlist_shuffle(current_tick, idx);
    playlist_start(idx, 0, 0);
    push_kind(V_NOW_PLAYING);
}

static void toggle_pause(void)
{
    int st = audio_status();
    if (st & AUDIO_STATUS_PAUSE)
        audio_resume();
    else if (st & AUDIO_STATUS_PLAY)
        audio_pause();
    mark_full();
}

/* ---- loop ---- */

static void render(void)
{
    struct view *v = top();

    switch (v->kind) {
    case V_HOME:
        if (need_full || drawn_sel != v->sel) {
            home_render();
            drawn_sel = v->sel;
            need_full = false;
        }
        break;

    case V_COVERFLOW:
        list_clamp(v);
        if (need_full || v->sel != drawn_sel) {
            draw_coverflow(v);
            need_full = false;
            drawn_sel = v->sel;
        }
        break;

    case V_NOW_PLAYING: {
        struct np_info np;
        shell_np_info(&np);
        if (!need_full && volume_dirty) {
            draw_status(view_title(v), true);
            volume_dirty = false;
            break;
        }
        draw_now_playing(&np, need_full);
        need_full = false;
        volume_dirty = false;
        break;
    }

    default:
        list_clamp(v);
        if (need_full || v->top != drawn_top) {
            draw_list(v, need_full);
            need_full = false;
        } else if (v->sel != drawn_sel) {
            /* selection moved inside the visible window: two rows, not the screen */
            draw_list_rows(v, drawn_sel, v->sel);
        }
        drawn_sel = v->sel;
        drawn_top = v->top;

        if (queued_tick && TIME_BEFORE(current_tick, queued_tick + HZ)) {
            draw_toast("Added to queue");
        } else if (queued_tick) {
            queued_tick = 0;
            mark_full();
        }
        if (overlay_letter) {
            if (TIME_BEFORE(current_tick, overlay_until))
                draw_letter_overlay(overlay_letter);
            else {
                overlay_letter = 0;
                mark_full();
            }
        }
        break;
    }
    gfx_flush();
}

static int timeout_for(const struct view *v)
{
    if (v->kind == V_NOW_PLAYING)
        return HZ / 2;
    if (overlay_letter)
        return overlay_until - current_tick > 0 ? overlay_until - current_tick : 1;
    return HZ;
}

static void handle(enum action a)
{
    struct view *v = top();

    if (a != A_PREV && a != A_NEXT && a != A_TIMEOUT && a != A_NONE)
        overlay_letter = 0;

    switch (a) {
    case A_BACK:
        pop();
        return;
    case A_HOME:
        depth = 1;
        art_cancel_pending();
        mark_full();
        return;
    case A_PLAY:
        toggle_pause();
        return;
    case A_USB:
        db_load();
        reset_to_home();
        return;
    default:
        break;
    }

    if (v->kind == V_HOME) {
        if (a == A_UP || a == A_DOWN) {
            const struct home_item *items[HOME_ALL];
            int n = home_items(items);
            (void)input_wheel_steps(); /* the home menu is short: one row per click */
            v->sel += a == A_DOWN ? 1 : -1;
            if (v->sel < 0)
                v->sel = 0;
            if (v->sel >= n)
                v->sel = n - 1;
        } else if (a == A_SELECT) {
            home_select();
        }
        return;
    }

    if (v->kind == V_NOW_PLAYING) {
        struct mp3entry *id3 = audio_current_track();
        switch (a) {
        case A_UP:
        case A_DOWN: {
            /* A full repaint per click (~26 ms on the 7G) is slower than the wheel
             * emits events, so clicks were being dropped and the volume crawled.
             * Repaint just the status bar and let a fast spin move several steps. */
            int step = input_wheel_steps();
            if (step > 8)
                step = 8; /* volume has far fewer steps than a track list */
            adjust_volume(a == A_DOWN ? step : -step);
            volume_dirty = true;
            break;
        }
        case A_NEXT: audio_next(); mark_full(); break;
        case A_PREV:
            if (id3 && id3->elapsed > 3000)
                audio_ff_rewind(0);
            else
                audio_prev();
            mark_full();
            break;
        case A_SEEK_BACK:
        case A_SEEK_FWD: {
            /* Accumulate an offset while the key is held and seek once on release.
             * Seeking on every repeat makes the engine rebuffer each time, which is
             * what made fast forward stutter and skip unevenly. */
            if (!id3 || !id3->length)
                break;
            if (!seeking) {
                seeking = true;
                seek_offset = 0;
                seek_step = 1000; /* ms per repeat, grows while held */
                audio_pre_ff_rewind();
            }
            long dir = (a == A_SEEK_FWD) ? 1 : -1;
            /* Cap each step against the distance left, so the end of a track is
             * approached smoothly instead of being overshot in one jump. */
            long room = dir > 0 ? (long)id3->length - ((long)id3->elapsed + seek_offset)
                                : (long)id3->elapsed + seek_offset;
            long step = MIN(seek_step, MAX(room / 4, 1000));
            seek_offset += step * dir;
            if ((long)id3->elapsed + seek_offset < 0)
                seek_offset = -(long)id3->elapsed;
            if ((long)id3->elapsed + seek_offset > (long)id3->length)
                seek_offset = (long)id3->length - (long)id3->elapsed;
            seek_step += seek_step >> 2; /* 1.25x per repeat */
            if (seek_step > 30000)
                seek_step = 30000;
            volume_dirty = false;
            draw_now_playing_seek(seek_target());
            gfx_flush();
            break;
        }
        case A_RATE: {
            if (current_uid == IPDB_NONE)
                break;
            current_rating = (uint8_t)((current_rating + 1) % 6);
            journal_log(JOURNAL_RATING, current_uid, current_rating);
            struct np_info np2;
            shell_np_info(&np2);
            draw_rating_overlay(&np2);
            gfx_flush();
            break;
        }
        case A_SELECT: /* cycle shuffle, then repeat, as the stock centre button does */
            if (!global_settings.playlist_shuffle) {
                global_settings.playlist_shuffle = true;
            } else {
                global_settings.playlist_shuffle = false;
                global_settings.repeat_mode = (global_settings.repeat_mode + 1) % NUM_REPEAT_MODES;
                audio_flush_and_reload_tracks();
            }
            settings_save();
            mark_full();
            break;
        default: break;
        }
        return;
    }

    switch (a) {
    case A_UP:   list_move(v, -input_wheel_steps()); break;
    case A_DOWN: list_move(v, input_wheel_steps()); break;
    case A_PREV: letter_jump(v, -1); break;
    case A_NEXT: letter_jump(v, 1); break;
    case A_RATE: /* centre held on a list: add to the queue, as stock On-The-Go did */
        if (view_is_tracks(v) && view_count(v) > 0 && playing()) {
            const ipdb_track *t = ipdb_track_at(&shell_db, view_track_id(v, v->sel));
            playlist_insert_track(NULL, ipdb_str(&shell_db, t->path), PLAYLIST_INSERT_LAST,
                                  false, true);
            queued_tick = current_tick;
            mark_full();
        }
        break;
    case A_SELECT: {
        if (view_count(v) == 0)
            break;
        if (v->kind == V_SETTINGS) {
            settings_select(v->sel);
            break;
        }
        struct view child;
        if (view_is_tracks(v))
            play_view(v, v->sel);
        else if (view_child(v, v->sel, &child))
            push(&child);
        break;
    }
    default:
        break;
    }
}

void shell_main(void)
{
    shell_boot_tick = current_tick;
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, false, NULL);
    input_init();
    prefs_load();
    shell_link_setup();

    if (!gfx_init()) {
        /* Without fonts the shell cannot draw anything legible. */
        FOR_NB_SCREENS(i)
            viewportmanager_theme_enable(i, true, NULL);
        root_menu();
    }

    int err = db_load();
    if (err != 0) {
        draw_message("No library found",
                     err < 0 ? "Sync from the companion to " SHELL_DIR : ipdb_strerror(err));
        gfx_flush();
        sleep(HZ * 3);
        enter_rockbox_ui();
    }
    art_init(shell_db.generation);
    journal_init();

    /* Pick up where the last session stopped, as the stock firmware does. */
    if (global_status.resume_index != -1) {
        if (playlist_resume() != -1) {
            playlist_resume_track(global_status.resume_index, global_status.resume_crc32,
                                  global_status.resume_elapsed, global_status.resume_offset);
        }
    }

    reset_to_home();
    for (;;) {
        render();
        enum action a = input_get(timeout_for(top()));
        if (a == A_QUIT) {
#ifdef SIMULATOR
            exit(0);
#endif
            continue;
        }
        journal_poll();
        resume_poll();
        if (art_take_dirty())
            mark_full(); /* art arrived: repaint so it appears */
        else if (a == A_TIMEOUT && (shell_show_clock() || shell_show_battery_pct()))
            mark_full(); /* keep the status bar readouts current */
        handle(a);

        /* Drain anything that arrived while we were drawing, so a fast wheel does not
         * queue up one repaint per click. */
        int drained = 0;
        while (input_pending() && drained < 64) {
            enum action next = input_get(0);
            if (next == A_NONE || next == A_TIMEOUT)
                break;
            handle(next);
            drained++;
        }
    }
}
