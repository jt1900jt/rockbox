/*
 * iPod OS link protocol, device side (docs/link-protocol.md in the iPodOS repo).
 * Transport-agnostic: feed received bytes with link_feed(); replies go through io.write.
 * No Rockbox dependencies, so the same code runs in the host test harness.
 */
#ifndef LINK_PROTO_H
#define LINK_PROTO_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define LINK_MAGIC0 'I'
#define LINK_MAGIC1 'P'
#define LINK_HDR 16
#define LINK_VERSION 1

#define LINK_F_CRC 0x01

enum link_type {
    /* transport tests */
    LINK_HELLO = 0x01,
    LINK_SINK = 0x02,
    LINK_SOURCE = 0x03,
    LINK_ECHO = 0x04,
    /* file operations (sync) */
    LINK_STAT = 0x10,      /* path -> STAT_R */
    LINK_LIST = 0x11,      /* path -> LIST_R */
    LINK_MKDIR = 0x12,     /* path */
    LINK_PUT_BEGIN = 0x13, /* u32 size, u32 crc, u8 flags, path */
    LINK_PUT_DATA = 0x14,  /* bytes */
    LINK_PUT_END = 0x15,   /* none: commit the staged file */
    LINK_GET = 0x16,       /* path -> DATA frames then GET_DONE */
    LINK_DELETE = 0x17,    /* path */
    LINK_FREE = 0x18,      /* none -> FREE_R */
    LINK_SYNC_DONE = 0x19, /* none: flush and reload the library */

    LINK_HELLO_R = 0x81,
    LINK_SINK_ACK = 0x82,
    LINK_DATA = 0x83,
    LINK_ECHO_R = 0x84,
    LINK_SOURCE_DONE = 0x85,
    LINK_OK = 0x86,        /* generic success, no payload */
    LINK_STAT_R = 0x87,    /* u8 kind (0 none, 1 file, 2 dir), u32 size, u32 mtime */
    LINK_LIST_R = 0x88,    /* repeated: u8 kind, u32 size, u32 mtime, u16 name_len, name */
    LINK_GET_DONE = 0x89,  /* u32 bytes sent */
    LINK_FREE_R = 0x8A,    /* u64 free bytes, u64 total bytes */
    LINK_ERROR = 0xFF,
};

/* PUT flags */
#define LINK_PUT_F_CRC 0x01

#define LINK_PATH_MAX 512

#define LINK_ECHO_MAX 65536
#define LINK_CHUNK_MAX 16384

/* Filesystem operations, supplied by the device. All paths are absolute and already
 * length-checked. Returning a negative value makes the frame fail with LINK_ERROR.
 * NULL means the transport is running without filesystem access (the test harness),
 * and every file operation is refused. */
struct link_fs {
    /* kind: 0 missing, 1 file, 2 dir */
    int (*stat)(void *ctx, const char *path, int *kind, uint32_t *size, uint32_t *mtime);
    /* Walks a directory, calling emit() per entry. */
    int (*list)(void *ctx, const char *path,
                void (*emit)(void *emit_ctx, const char *name, int kind, uint32_t size, uint32_t mtime),
                void *emit_ctx);
    int (*mkdir)(void *ctx, const char *path);
    int (*remove)(void *ctx, const char *path);
    /* Staged write: open a temporary file, append, then rename over the target. */
    int (*put_begin)(void *ctx, const char *path, uint32_t size);
    int (*put_data)(void *ctx, const void *buf, size_t n);
    int (*put_end)(void *ctx, bool commit);
    /* Reads a file, handing chunks to send(); return the byte count or negative. */
    int (*get)(void *ctx, const char *path, int (*send)(void *send_ctx, const void *buf, size_t n),
               void *send_ctx);
    int (*freespace)(void *ctx, uint64_t *freebytes, uint64_t *total);
    void (*sync_done)(void *ctx);
    void *ctx;
};

struct link_io {
    /* Write all n bytes; return 0, or -1 if the link is gone. May block. */
    int (*write)(void *ctx, const void *buf, size_t n);
    void *ctx;
    const char *hello; /* device description returned in HELLO_R */
    const struct link_fs *fs;
};

struct link_stats {
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint32_t frames;
    uint32_t crc_errors;
    uint32_t resyncs;
    uint32_t errors;
};

struct link {
    struct link_io io;
    struct link_stats st;
    uint8_t hdr[LINK_HDR];
    size_t hdr_have;
    int in_payload;
    uint8_t type, flags;
    uint32_t seq, len, remaining, crc_expect, crc_run;
    uint8_t small[16];
    size_t small_have;
    uint8_t echo[LINK_ECHO_MAX];

    /* file operation state */
    char path[LINK_PATH_MAX];
    size_t path_have;
    bool put_open;
    bool put_bad;
    uint32_t put_expect_crc;
    uint32_t put_crc;
    uint8_t put_flags;
    uint32_t put_written;
    uint8_t chunk[LINK_CHUNK_MAX];
    size_t chunk_have;
};

void link_init(struct link *l, const struct link_io *io);
void link_feed(struct link *l, const uint8_t *data, size_t n);

#endif
