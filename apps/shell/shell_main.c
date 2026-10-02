/* Shell entry: loads the library, runs the navigation stack, starts playback.
 * Falls back to the stock Rockbox UI when no valid library is present. */
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
#include "viewport.h"

#include "shell.h"
#include "shell_main.h"

#define STACK_MAX   12
#define MAX_DB_SIZE (48u * 1024 * 1024)
#define LETTER_TICKS (HZ * 3 / 4)

ipdb_db shell_db;
static int db_handle = -1;

static struct view stack[STACK_MAX];
static int depth;
static long overlay_until;
static char overlay_letter;

/* ---- library ---- */

static void db_unload(void)
{
    if (db_handle > 0)
        core_free(db_handle);
    db_handle = -1;
    memset(&shell_db, 0, sizeof shell_db);
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
    int err = ipdb_open(&shell_db, buf, (size_t)size);
    if (err != IPDB_OK) {
        core_free(h);
        return err;
    }
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
}

static bool playing(void)
{
    return (audio_status() & AUDIO_STATUS_PLAY) != 0;
}

/* ---- home ---- */

struct home_item {
    const char *label;
    enum view_kind kind;
    bool rockbox;
};

static const struct home_item home_all[] = {
    { "PLAYLISTS", V_PLAYLISTS, false },
    { "ARTISTS", V_ARTISTS, false },
    { "ALBUMS", V_ALBUMS, false },
    { "SONGS", V_SONGS, false },
    { "GENRES", V_GENRES, false },
    { "COMPOSERS", V_COMPOSERS, false },
    { "NOW PLAYING", V_NOW_PLAYING, false },
    { "ROCKBOX", V_HOME, true },
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
    draw_home(stack[0].sel, labels, n);
}

static void enter_rockbox_ui(void)
{
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

/* ---- lists ---- */

static void list_clamp(struct view *v)
{
    int n = view_count(v);
    if (v->sel >= n)
        v->sel = n - 1;
    if (v->sel < 0)
        v->sel = 0;
    if (v->top > v->sel)
        v->top = v->sel;
    if (v->sel >= v->top + SHELL_LIST_ROWS)
        v->top = v->sel - SHELL_LIST_ROWS + 1;
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
}

/* ---- loop ---- */

static void render(void)
{
    struct view *v = top();
    switch (v->kind) {
    case V_HOME:
        home_render();
        break;
    case V_NOW_PLAYING:
        draw_now_playing();
        break;
    default:
        list_clamp(v);
        draw_list(v);
        if (overlay_letter && TIME_BEFORE(current_tick, overlay_until))
            draw_letter_overlay(overlay_letter);
        else
            overlay_letter = 0;
        break;
    }
}

static int timeout_for(const struct view *v)
{
    if (v->kind == V_NOW_PLAYING)
        return HZ / 4;
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
        if (depth > 1)
            depth--;
        return;
    case A_HOME:
        depth = 1;
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
        case A_UP:   adjust_volume(-1); break;
        case A_DOWN: adjust_volume(1); break;
        case A_NEXT: audio_next(); break;
        case A_PREV:
            if (id3 && id3->elapsed > 3000)
                audio_ff_rewind(0);
            else
                audio_prev();
            break;
        default: break;
        }
        return;
    }

    switch (a) {
    case A_UP:   list_move(v, -1); break;
    case A_DOWN: list_move(v, 1); break;
    case A_PREV: letter_jump(v, -1); break;
    case A_NEXT: letter_jump(v, 1); break;
    case A_SELECT: {
        if (view_count(v) == 0)
            break;
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
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, false, NULL);
    input_init();
    draw_init();

    int err = db_load();
    if (err != 0) {
        draw_message("No library found",
                     err < 0 ? "Sync from the companion to " SHELL_DIR : ipdb_strerror(err));
        sleep(HZ * 3);
        enter_rockbox_ui();
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
        handle(a);
    }
}
