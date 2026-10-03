/*
 * Device-side reader for library.ipdb and artwork.ipap (docs/ipdb-format.md).
 *
 * library.ipdb: load the whole file into a 4-byte-aligned buffer, call ipdb_open().
 * On IPDB_OK every ID, index and string offset reachable through the accessors below
 * has been bounds-checked, so accessors do no further checking.
 *
 * artwork.ipap: read the first IPAP_HEAD_MAX bytes, call ipap_parse(), then use
 * ipap_slot() to get the file offset of an image and read it yourself.
 *
 * No allocation, no I/O, no libc beyond memcmp/memcpy. Little-endian hosts only.
 */
#ifndef IPDB_H
#define IPDB_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "ipdb reader assumes a little-endian host"
#endif

#define IPDB_NONE 0xFFFFFFFFu
#define IPDB_VERSION_MAJOR 1
#define IPDB_JUMP_BUCKETS 27 /* A..Z, then '#' */

enum {
    IPDB_JUMP_SONGS,
    IPDB_JUMP_ALBUMS,
    IPDB_JUMP_ARTISTS,
    IPDB_JUMP_GENRES,
    IPDB_JUMP_COMPOSERS,
    IPDB_JUMP_ROWS
};

enum ipdb_err {
    IPDB_OK = 0,
    IPDB_E_ALIGN,   /* buffer not 4-byte aligned */
    IPDB_E_SHORT,   /* buffer shorter than header or file_size */
    IPDB_E_MAGIC,
    IPDB_E_VERSION,
    IPDB_E_HEADER,  /* bad header_size / section table */
    IPDB_E_BOUNDS,  /* a section lies outside the file or is misaligned */
    IPDB_E_CRC,
    IPDB_E_MISSING, /* a required section is absent */
    IPDB_E_SIZE,    /* section size does not match its count */
    IPDB_E_STRINGS, /* string pool not NUL-bounded or offset out of range */
    IPDB_E_REF      /* an ID or index entry is out of range */
};

enum ipdb_codec {
    IPDB_CODEC_UNKNOWN, IPDB_CODEC_MP3, IPDB_CODEC_AAC, IPDB_CODEC_ALAC, IPDB_CODEC_FLAC,
    IPDB_CODEC_VORBIS, IPDB_CODEC_OPUS, IPDB_CODEC_WAV, IPDB_CODEC_AIFF, IPDB_CODEC_WAVPACK,
    IPDB_CODEC_APE, IPDB_CODEC_MUSEPACK
};

#define IPDB_TF_RG_TRACK    0x01
#define IPDB_TF_RG_ALBUM    0x02
#define IPDB_TF_COMPILATION 0x04
#define IPDB_TF_LOSSLESS    0x08
#define IPDB_TF_AUDIOBOOK   0x10

#define IPDB_AF_COMPILATION 0x01

typedef struct {
    uint32_t uid;
    uint32_t path;   /* string offset */
    uint32_t title;  /* string offset */
    uint32_t artist_id;
    uint32_t album_id;
    uint32_t genre_id;    /* or IPDB_NONE */
    uint32_t composer_id; /* or IPDB_NONE */
    uint32_t duration_ms;
    uint32_t sample_rate;
    uint32_t file_size;
    uint32_t mtime;
    uint16_t track_no;
    uint16_t disc_no;
    uint16_t year;
    uint16_t bitrate_kbps;
    int16_t rg_track_cdb; /* hundredths of a dB */
    int16_t rg_album_cdb;
    uint8_t codec;
    uint8_t bits;
    uint8_t channels;
    uint8_t flags;
    uint32_t reserved;
} ipdb_track;

typedef struct {
    uint32_t title;
    uint32_t artist_id;
    uint32_t art_id; /* or IPDB_NONE */
    uint32_t tracks_first;
    uint32_t tracks_count;
    uint16_t year;
    uint8_t flags;
    uint8_t reserved;
    uint16_t colors[3]; /* RGB565, most prominent first */
    uint16_t reserved2;
} ipdb_album;

/* Artist: first/count -> IART (albums), extra = track count
 * Genre:  first/count -> IGEN (artists), extra = track count
 * Composer / playlist: first/count -> ICMP / IPLS (tracks) */
typedef struct {
    uint32_t name;
    uint32_t first;
    uint32_t count;
    uint32_t extra;
} ipdb_group;

typedef struct {
    uint64_t generation;
    uint64_t created;
    const char *strs;
    uint32_t strs_size;
    const ipdb_track *tracks;
    uint32_t n_tracks;
    const ipdb_album *albums;
    uint32_t n_albums;
    const ipdb_group *artists, *genres, *composers, *playlists;
    uint32_t n_artists, n_genres, n_composers, n_playlists;
    const uint32_t *album_tracks, *artist_albums, *genre_artists, *composer_tracks, *playlist_tracks;
    uint32_t n_album_tracks, n_artist_albums, n_genre_artists, n_composer_tracks, n_playlist_tracks;
    const uint32_t *jump; /* IPDB_JUMP_ROWS x IPDB_JUMP_BUCKETS */
} ipdb_db;

int ipdb_open(ipdb_db *db, const void *buf, size_t len);

/* As ipdb_open, but optionally trusts the stored CRC instead of recomputing it.
 * The CRC is the largest part of load time on a large library; the structural checks,
 * which are what make the accessors bounds-safe, always run. Skip it only when the file
 * is known unchanged since a previous verified load. */
int ipdb_open_ex(ipdb_db *db, const void *buf, size_t len, bool skip_crc);

/* Header fields readable before validation, for deciding whether to skip the CRC.
 * Both return 0 if the buffer is too small or the magic is wrong. */
uint64_t ipdb_peek_generation(const void *buf, size_t len);
uint32_t ipdb_peek_crc(const void *buf, size_t len);
const char *ipdb_strerror(int err);
uint32_t ipdb_crc32(uint32_t crc, const void *data, size_t len);

static inline const char *ipdb_str(const ipdb_db *db, uint32_t off) { return db->strs + off; }
static inline const ipdb_track *ipdb_track_at(const ipdb_db *db, uint32_t id) { return &db->tracks[id]; }
static inline const ipdb_album *ipdb_album_at(const ipdb_db *db, uint32_t id) { return &db->albums[id]; }

/* Track id of the k-th track in album `a` (0 <= k < tracks_count). */
static inline uint32_t ipdb_album_track(const ipdb_db *db, const ipdb_album *a, uint32_t k)
{
    return db->album_tracks[a->tracks_first + k];
}

/* First list index for a jump bucket (0..26). Bucket b spans [jump(b), jump(b+1)). */
static inline uint32_t ipdb_jump(const ipdb_db *db, int row, int bucket)
{
    return db->jump[row * IPDB_JUMP_BUCKETS + bucket];
}

/* ---- artwork.ipap ---- */

#define IPAP_MAX_CLASSES 8
#define IPAP_HEAD_MAX (64 + 16 * IPAP_MAX_CLASSES)
#define IPAP_FOURCC(a, b, c, d) \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))
#define IPAP_THMB IPAP_FOURCC('T', 'H', 'M', 'B')
#define IPAP_HEAD IPAP_FOURCC('H', 'E', 'A', 'D')
#define IPAP_LRGE IPAP_FOURCC('L', 'R', 'G', 'E')
#define IPAP_BLUR IPAP_FOURCC('B', 'L', 'U', 'R')

typedef struct {
    uint32_t id;
    uint16_t width;
    uint16_t height;
    uint64_t offset;
} ipap_class;

typedef struct {
    uint32_t art_count;
    uint32_t data_offset;
    uint64_t generation;
    uint64_t file_size;
    uint32_t class_count;
    ipap_class classes[IPAP_MAX_CLASSES];
} ipap_pack;

/* head: the first min(file length, IPAP_HEAD_MAX) bytes of the file. */
int ipap_parse(ipap_pack *p, const void *head, size_t head_len, uint64_t file_len);

/* Locate an image. Returns 0 on success, -1 if the class is absent or art_id is out of range.
 * The slot is width*height RGB565 little-endian pixels, row-major. */
int ipap_slot(const ipap_pack *p, uint32_t class_id, uint32_t art_id,
              uint64_t *offset, uint32_t *size, uint16_t *width, uint16_t *height);

#endif
