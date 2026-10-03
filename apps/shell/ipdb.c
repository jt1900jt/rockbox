#include "ipdb.h"

#include <string.h>

/* Compile-time layout checks (C99-compatible). */
#define IPDB_ASSERT(name, cond) typedef char ipdb_assert_##name[(cond) ? 1 : -1]
IPDB_ASSERT(track_size, sizeof(ipdb_track) == 64);
IPDB_ASSERT(track_codec, offsetof(ipdb_track, codec) == 56);
IPDB_ASSERT(album_size, sizeof(ipdb_album) == 32);
IPDB_ASSERT(album_colors, offsetof(ipdb_album, colors) == 24);
IPDB_ASSERT(group_size, sizeof(ipdb_group) == 16);
IPDB_ASSERT(class_size, sizeof(ipap_class) == 16);

#define HEADER_SIZE 64u
#define ENTRY_SIZE 16u
#define MAX_SECTIONS 4096u

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* CRC-32 (IEEE), slice-by-8. Chains like zlib: crc32(crc32(0, a), b) == crc32(0, a||b).
 * Byte loads keep it alignment-agnostic (ARM926 faults on unaligned word loads). */
uint32_t ipdb_crc32(uint32_t crc, const void *data, size_t len)
{
    static uint32_t t[8][256];
    static int ready;
    const uint8_t *p = data;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; i++)
            for (int s = 1; s < 8; s++)
                t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
        ready = 1;
    }
    crc = ~crc;
    while (len >= 8) {
        uint32_t lo = crc ^ ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
        uint32_t hi = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        crc = t[7][lo & 0xFF] ^ t[6][(lo >> 8) & 0xFF] ^ t[5][(lo >> 16) & 0xFF] ^ t[4][lo >> 24] ^
              t[3][hi & 0xFF] ^ t[2][(hi >> 8) & 0xFF] ^ t[1][(hi >> 16) & 0xFF] ^ t[0][hi >> 24];
        p += 8;
        len -= 8;
    }
    while (len--)
        crc = t[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

const char *ipdb_strerror(int err)
{
    static const char *const msgs[] = {
        "ok", "buffer misaligned", "file truncated", "bad magic", "unsupported version",
        "bad header", "section out of bounds", "checksum mismatch", "missing section",
        "section size mismatch", "bad string pool", "reference out of range",
    };
    return (err >= 0 && err < (int)(sizeof msgs / sizeof msgs[0])) ? msgs[err] : "unknown error";
}

enum { S_STRS, S_TRKS, S_ALBM, S_ARTS, S_GENR, S_COMP, S_PLST, S_IALB, S_IART, S_IGEN, S_ICMP, S_IPLS, S_JUMP, S_COUNT };

static const struct {
    char id[4];
    uint32_t recsize;
} wanted[S_COUNT] = {
    { "STRS", 1 }, { "TRKS", 64 }, { "ALBM", 32 }, { "ARTS", 16 }, { "GENR", 16 }, { "COMP", 16 },
    { "PLST", 16 }, { "IALB", 4 }, { "IART", 4 }, { "IGEN", 4 }, { "ICMP", 4 }, { "IPLS", 4 },
    { "JUMP", IPDB_JUMP_BUCKETS * 4 },
};

struct sec {
    const uint8_t *p;
    uint32_t size;
    uint32_t count;
    int found;
};

/* [first, first + count) lies within [0, n). */
static int span_ok(uint32_t first, uint32_t count, uint32_t n)
{
    return first <= n && count <= n - first;
}

static int all_below(const uint32_t *v, uint32_t n, uint32_t max)
{
    for (uint32_t i = 0; i < n; i++)
        if (v[i] >= max)
            return 0;
    return 1;
}

static int groups_ok(const ipdb_group *g, uint32_t n, uint32_t strs_size, uint32_t list_len)
{
    for (uint32_t i = 0; i < n; i++)
        if (g[i].name >= strs_size || !span_ok(g[i].first, g[i].count, list_len))
            return 0;
    return 1;
}

uint64_t ipdb_peek_generation(const void *buf, size_t len)
{
    const uint8_t *b = buf;
    if (len < HEADER_SIZE || memcmp(b, "IPDB", 4) != 0)
        return 0;
    return rd64(b + 24);
}

uint32_t ipdb_peek_crc(const void *buf, size_t len)
{
    const uint8_t *b = buf;
    if (len < HEADER_SIZE || memcmp(b, "IPDB", 4) != 0)
        return 0;
    return rd32(b + 40);
}

int ipdb_open(ipdb_db *db, const void *buf, size_t len)
{
    return ipdb_open_ex(db, buf, len, false);
}

int ipdb_open_ex(ipdb_db *db, const void *buf, size_t len, bool skip_crc)
{
    const uint8_t *b = buf;
    struct sec s[S_COUNT];
    memset(s, 0, sizeof s);
    memset(db, 0, sizeof *db);

    if ((uintptr_t)b & 3)
        return IPDB_E_ALIGN;
    if (len < HEADER_SIZE)
        return IPDB_E_SHORT;
    if (memcmp(b, "IPDB", 4) != 0)
        return IPDB_E_MAGIC;
    if (rd16(b + 4) != IPDB_VERSION_MAJOR)
        return IPDB_E_VERSION;

    uint32_t hsize = rd32(b + 8), nsec = rd32(b + 12), table = rd32(b + 16), fsize = rd32(b + 20);
    if (hsize != HEADER_SIZE)
        return IPDB_E_HEADER;
    if (fsize > len || fsize < HEADER_SIZE)
        return IPDB_E_SHORT;
    if (nsec > MAX_SECTIONS || table < HEADER_SIZE || table > fsize || (fsize - table) / ENTRY_SIZE < nsec)
        return IPDB_E_HEADER;
    if (!skip_crc && ipdb_crc32(0, b + HEADER_SIZE, fsize - HEADER_SIZE) != rd32(b + 40))
        return IPDB_E_CRC;

    uint32_t table_end = table + nsec * ENTRY_SIZE;
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *e = b + table + i * ENTRY_SIZE;
        uint32_t off = rd32(e + 4), size = rd32(e + 8), count = rd32(e + 12);
        if ((off & 15) || off < table_end || off > fsize || size > fsize - off)
            return IPDB_E_BOUNDS;
        for (int k = 0; k < S_COUNT; k++) {
            if (memcmp(e, wanted[k].id, 4) == 0) {
                if (s[k].found)
                    return IPDB_E_HEADER;
                s[k].p = b + off;
                s[k].size = size;
                s[k].count = count;
                s[k].found = 1;
            }
        }
    }
    for (int k = 0; k < S_COUNT; k++) {
        if (!s[k].found)
            return IPDB_E_MISSING;
        if (k != S_STRS && (uint64_t)s[k].count * wanted[k].recsize != s[k].size)
            return IPDB_E_SIZE;
    }
    if (s[S_JUMP].count != IPDB_JUMP_ROWS)
        return IPDB_E_SIZE;
    if (s[S_STRS].size == 0 || s[S_STRS].p[0] != 0 || s[S_STRS].p[s[S_STRS].size - 1] != 0)
        return IPDB_E_STRINGS;

    db->generation = rd64(b + 24);
    db->created = rd64(b + 32);
    db->strs = (const char *)s[S_STRS].p;
    db->strs_size = s[S_STRS].size;
    db->tracks = (const ipdb_track *)s[S_TRKS].p;
    db->n_tracks = s[S_TRKS].count;
    db->albums = (const ipdb_album *)s[S_ALBM].p;
    db->n_albums = s[S_ALBM].count;
    db->artists = (const ipdb_group *)s[S_ARTS].p;
    db->n_artists = s[S_ARTS].count;
    db->genres = (const ipdb_group *)s[S_GENR].p;
    db->n_genres = s[S_GENR].count;
    db->composers = (const ipdb_group *)s[S_COMP].p;
    db->n_composers = s[S_COMP].count;
    db->playlists = (const ipdb_group *)s[S_PLST].p;
    db->n_playlists = s[S_PLST].count;
    db->album_tracks = (const uint32_t *)s[S_IALB].p;
    db->n_album_tracks = s[S_IALB].count;
    db->artist_albums = (const uint32_t *)s[S_IART].p;
    db->n_artist_albums = s[S_IART].count;
    db->genre_artists = (const uint32_t *)s[S_IGEN].p;
    db->n_genre_artists = s[S_IGEN].count;
    db->composer_tracks = (const uint32_t *)s[S_ICMP].p;
    db->n_composer_tracks = s[S_ICMP].count;
    db->playlist_tracks = (const uint32_t *)s[S_IPLS].p;
    db->n_playlist_tracks = s[S_IPLS].count;
    db->jump = (const uint32_t *)s[S_JUMP].p;

    int err = IPDB_E_REF;
    for (uint32_t i = 0; i < db->n_tracks; i++) {
        const ipdb_track *t = &db->tracks[i];
        if (t->path >= db->strs_size || t->title >= db->strs_size) {
            err = IPDB_E_STRINGS;
            goto fail;
        }
        if (t->artist_id >= db->n_artists || t->album_id >= db->n_albums ||
            (t->genre_id != IPDB_NONE && t->genre_id >= db->n_genres) ||
            (t->composer_id != IPDB_NONE && t->composer_id >= db->n_composers))
            goto fail;
    }
    for (uint32_t i = 0; i < db->n_albums; i++) {
        const ipdb_album *a = &db->albums[i];
        if (a->title >= db->strs_size) {
            err = IPDB_E_STRINGS;
            goto fail;
        }
        if (a->artist_id >= db->n_artists || !span_ok(a->tracks_first, a->tracks_count, db->n_album_tracks))
            goto fail;
    }
    if (!groups_ok(db->artists, db->n_artists, db->strs_size, db->n_artist_albums) ||
        !groups_ok(db->genres, db->n_genres, db->strs_size, db->n_genre_artists) ||
        !groups_ok(db->composers, db->n_composers, db->strs_size, db->n_composer_tracks) ||
        !groups_ok(db->playlists, db->n_playlists, db->strs_size, db->n_playlist_tracks))
        goto fail;
    if (!all_below(db->album_tracks, db->n_album_tracks, db->n_tracks) ||
        !all_below(db->artist_albums, db->n_artist_albums, db->n_albums) ||
        !all_below(db->genre_artists, db->n_genre_artists, db->n_artists) ||
        !all_below(db->composer_tracks, db->n_composer_tracks, db->n_tracks) ||
        !all_below(db->playlist_tracks, db->n_playlist_tracks, db->n_tracks))
        goto fail;

    const uint32_t row_len[IPDB_JUMP_ROWS] = { db->n_tracks, db->n_albums, db->n_artists, db->n_genres, db->n_composers };
    for (int r = 0; r < IPDB_JUMP_ROWS; r++) {
        uint32_t prev = 0;
        for (int k = 0; k < IPDB_JUMP_BUCKETS; k++) {
            uint32_t v = ipdb_jump(db, r, k);
            if (v > row_len[r] || v < prev)
                goto fail;
            prev = v;
        }
    }
    return IPDB_OK;

fail:
    memset(db, 0, sizeof *db);
    return err;
}

int ipap_parse(ipap_pack *p, const void *head, size_t head_len, uint64_t file_len)
{
    const uint8_t *h = head;
    static const uint8_t zero4[4];
    memset(p, 0, sizeof *p);

    if (head_len < HEADER_SIZE)
        return IPDB_E_SHORT;
    if (memcmp(h, "IPAP", 4) != 0)
        return IPDB_E_MAGIC;
    if (rd16(h + 4) != IPDB_VERSION_MAJOR)
        return IPDB_E_VERSION;
    if (rd32(h + 8) != HEADER_SIZE)
        return IPDB_E_HEADER;
    uint32_t ncls = rd32(h + 12);
    if (ncls > IPAP_MAX_CLASSES)
        return IPDB_E_HEADER;
    uint32_t table_end = HEADER_SIZE + ncls * ENTRY_SIZE;
    if (head_len < table_end)
        return IPDB_E_SHORT;

    uint32_t crc = ipdb_crc32(0, h, 40);
    crc = ipdb_crc32(crc, zero4, 4);
    crc = ipdb_crc32(crc, h + 44, table_end - 44);
    if (crc != rd32(h + 40))
        return IPDB_E_CRC;

    p->art_count = rd32(h + 16);
    p->data_offset = rd32(h + 20);
    p->generation = rd64(h + 24);
    p->file_size = rd64(h + 32);
    p->class_count = ncls;
    if (p->file_size > file_len)
        return IPDB_E_SHORT;
    if (p->data_offset < table_end)
        return IPDB_E_BOUNDS;

    for (uint32_t i = 0; i < ncls; i++) {
        const uint8_t *e = h + HEADER_SIZE + i * ENTRY_SIZE;
        ipap_class *c = &p->classes[i];
        c->id = rd32(e);
        c->width = rd16(e + 4);
        c->height = rd16(e + 6);
        c->offset = rd64(e + 8);
        uint64_t slot = (uint64_t)c->width * c->height * 2;
        if (c->offset < p->data_offset || c->offset > p->file_size)
            return IPDB_E_BOUNDS;
        if (p->art_count && slot > (p->file_size - c->offset) / p->art_count)
            return IPDB_E_BOUNDS;
    }
    return IPDB_OK;
}

int ipap_slot(const ipap_pack *p, uint32_t class_id, uint32_t art_id,
              uint64_t *offset, uint32_t *size, uint16_t *width, uint16_t *height)
{
    if (art_id >= p->art_count)
        return -1;
    for (uint32_t i = 0; i < p->class_count; i++) {
        const ipap_class *c = &p->classes[i];
        if (c->id != class_id)
            continue;
        uint32_t slot = (uint32_t)c->width * c->height * 2;
        *offset = c->offset + (uint64_t)art_id * slot;
        *size = slot;
        *width = c->width;
        *height = c->height;
        return 0;
    }
    return -1;
}
