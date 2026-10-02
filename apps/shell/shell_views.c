/* List views over the library database. Every list is O(1) random access, so the
 * renderer only touches the rows on screen. */
#include <stdio.h>
#include <string.h>

#include "shell.h"

/* Artist -> album drill-down shows only that artist's tracks on the album. */
#define FILTER_MAX 1024
static uint32_t filtered[FILTER_MAX];
static int n_filtered;

static const ipdb_db *db = &shell_db;

static void plural(char *buf, size_t len, uint32_t n, const char *one, const char *many)
{
    snprintf(buf, len, "%lu %s", (unsigned long)n, n == 1 ? one : many);
}

static const ipdb_album *album_of_track(uint32_t t)
{
    return ipdb_album_at(db, ipdb_track_at(db, t)->album_id);
}

int view_count(const struct view *v)
{
    switch (v->kind) {
    case V_SONGS:               return (int)db->n_tracks;
    case V_ALBUMS:              return (int)db->n_albums;
    case V_ARTISTS:             return (int)db->n_artists;
    case V_GENRES:              return (int)db->n_genres;
    case V_COMPOSERS:           return (int)db->n_composers;
    case V_PLAYLISTS:           return (int)db->n_playlists;
    case V_ALBUM_TRACKS:        return (int)db->albums[v->arg].tracks_count;
    case V_ARTIST_ALBUMS:       return (int)db->artists[v->arg].count;
    case V_ARTIST_ALBUM_TRACKS: return n_filtered;
    case V_GENRE_ARTISTS:       return (int)db->genres[v->arg].count;
    case V_COMPOSER_TRACKS:     return (int)db->composers[v->arg].count;
    case V_PLAYLIST_TRACKS:     return (int)db->playlists[v->arg].count;
    default:                    return 0;
    }
}

bool view_is_tracks(const struct view *v)
{
    switch (v->kind) {
    case V_SONGS:
    case V_ALBUM_TRACKS:
    case V_ARTIST_ALBUM_TRACKS:
    case V_COMPOSER_TRACKS:
    case V_PLAYLIST_TRACKS:
        return true;
    default:
        return false;
    }
}

uint32_t view_track_id(const struct view *v, int i)
{
    switch (v->kind) {
    case V_SONGS:               return (uint32_t)i;
    case V_ALBUM_TRACKS:        return ipdb_album_track(db, &db->albums[v->arg], (uint32_t)i);
    case V_ARTIST_ALBUM_TRACKS: return filtered[i];
    case V_COMPOSER_TRACKS:     return db->composer_tracks[db->composers[v->arg].first + i];
    case V_PLAYLIST_TRACKS:     return db->playlist_tracks[db->playlists[v->arg].first + i];
    default:                    return IPDB_NONE;
    }
}

static uint32_t album_at(const struct view *v, int i)
{
    if (v->kind == V_ARTIST_ALBUMS)
        return db->artist_albums[db->artists[v->arg].first + i];
    return (uint32_t)i;
}

static uint32_t artist_at(const struct view *v, int i)
{
    if (v->kind == V_GENRE_ARTISTS)
        return db->genre_artists[db->genres[v->arg].first + i];
    return (uint32_t)i;
}

void view_row(const struct view *v, int i, struct row *r)
{
    memset(r, 0, sizeof *r);
    r->sub = r->subbuf;

    if (view_is_tracks(v)) {
        uint32_t tid = view_track_id(v, i);
        const ipdb_track *t = ipdb_track_at(db, tid);
        const ipdb_album *al = album_of_track(tid);
        const char *artist = ipdb_str(db, db->artists[t->artist_id].name);
        r->title = ipdb_str(db, t->title);
        if (v->kind == V_ALBUM_TRACKS || v->kind == V_ARTIST_ALBUM_TRACKS)
            snprintf(r->subbuf, sizeof r->subbuf, "%s", artist);
        else
            snprintf(r->subbuf, sizeof r->subbuf, "%s \xc2\xb7 %s", artist, ipdb_str(db, al->title));
        format_duration(r->trail, sizeof r->trail, t->duration_ms);
        r->has_thumb = true;
        r->swatch = al->art_id != IPDB_NONE ? al->colors[0] : 0;
        return;
    }

    switch (v->kind) {
    case V_ALBUMS:
    case V_ARTIST_ALBUMS: {
        const ipdb_album *al = ipdb_album_at(db, album_at(v, i));
        r->title = ipdb_str(db, al->title);
        if (v->kind == V_ARTIST_ALBUMS) {
            if (al->year)
                snprintf(r->subbuf, sizeof r->subbuf, "%u", al->year);
        } else {
            snprintf(r->subbuf, sizeof r->subbuf, "%s", ipdb_str(db, db->artists[al->artist_id].name));
        }
        r->has_thumb = true;
        r->swatch = al->art_id != IPDB_NONE ? al->colors[0] : 0;
        break;
    }
    case V_ARTISTS:
    case V_GENRE_ARTISTS: {
        const ipdb_group *g = &db->artists[artist_at(v, i)];
        r->title = ipdb_str(db, g->name);
        plural(r->subbuf, sizeof r->subbuf, g->count, "album", "albums");
        break;
    }
    case V_GENRES: {
        const ipdb_group *g = &db->genres[i];
        r->title = ipdb_str(db, g->name);
        plural(r->subbuf, sizeof r->subbuf, g->count, "artist", "artists");
        break;
    }
    case V_COMPOSERS:
    case V_PLAYLISTS: {
        const ipdb_group *g = v->kind == V_COMPOSERS ? &db->composers[i] : &db->playlists[i];
        r->title = ipdb_str(db, g->name);
        plural(r->subbuf, sizeof r->subbuf, g->count, "song", "songs");
        break;
    }
    default:
        r->title = "";
        break;
    }
}

const char *view_title(const struct view *v)
{
    switch (v->kind) {
    case V_SONGS:               return "Songs";
    case V_ALBUMS:              return "Albums";
    case V_ARTISTS:             return "Artists";
    case V_GENRES:              return "Genres";
    case V_COMPOSERS:           return "Composers";
    case V_PLAYLISTS:           return "Playlists";
    case V_ALBUM_TRACKS:
    case V_ARTIST_ALBUM_TRACKS: return ipdb_str(db, db->albums[v->arg].title);
    case V_ARTIST_ALBUMS:       return ipdb_str(db, db->artists[v->arg].name);
    case V_GENRE_ARTISTS:       return ipdb_str(db, db->genres[v->arg].name);
    case V_COMPOSER_TRACKS:     return ipdb_str(db, db->composers[v->arg].name);
    case V_PLAYLIST_TRACKS:     return ipdb_str(db, db->playlists[v->arg].name);
    case V_NOW_PLAYING:         return "Now Playing";
    default:                    return "";
    }
}

int view_jump_row(const struct view *v)
{
    switch (v->kind) {
    case V_SONGS:     return IPDB_JUMP_SONGS;
    case V_ALBUMS:    return IPDB_JUMP_ALBUMS;
    case V_ARTISTS:   return IPDB_JUMP_ARTISTS;
    case V_GENRES:    return IPDB_JUMP_GENRES;
    case V_COMPOSERS: return IPDB_JUMP_COMPOSERS;
    default:          return -1;
    }
}

bool view_child(const struct view *v, int i, struct view *child)
{
    memset(child, 0, sizeof *child);
    switch (v->kind) {
    case V_ALBUMS:
        child->kind = V_ALBUM_TRACKS;
        child->arg = (uint32_t)i;
        return true;
    case V_ARTIST_ALBUMS:
        child->kind = V_ARTIST_ALBUM_TRACKS;
        child->arg = album_at(v, i);
        child->arg2 = v->arg;
        return true;
    case V_ARTISTS:
    case V_GENRE_ARTISTS:
        child->kind = V_ARTIST_ALBUMS;
        child->arg = artist_at(v, i);
        return true;
    case V_GENRES:
        child->kind = V_GENRE_ARTISTS;
        child->arg = (uint32_t)i;
        return true;
    case V_COMPOSERS:
        child->kind = V_COMPOSER_TRACKS;
        child->arg = (uint32_t)i;
        return true;
    case V_PLAYLISTS:
        child->kind = V_PLAYLIST_TRACKS;
        child->arg = (uint32_t)i;
        return true;
    default:
        return false;
    }
}

void view_prepare(struct view *v)
{
    if (v->kind != V_ARTIST_ALBUM_TRACKS)
        return;
    /* Albums where the artist only guests show just their tracks; their own albums show everything. */
    const ipdb_album *al = &db->albums[v->arg];
    bool own = al->artist_id == v->arg2;
    n_filtered = 0;
    for (uint32_t k = 0; k < al->tracks_count && n_filtered < FILTER_MAX; k++) {
        uint32_t t = ipdb_album_track(db, al, k);
        if (own || db->tracks[t].artist_id == v->arg2)
            filtered[n_filtered++] = t;
    }
}
