/*
 * iPod OS link protocol, device side (docs/link-protocol.md in the iPodOS repo).
 * Transport-agnostic: feed received bytes with link_feed(); replies go through io.write.
 * No Rockbox dependencies, so the same code runs in the host test harness.
 */
#ifndef LINK_PROTO_H
#define LINK_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define LINK_MAGIC0 'I'
#define LINK_MAGIC1 'P'
#define LINK_HDR 16
#define LINK_VERSION 1

#define LINK_F_CRC 0x01

enum link_type {
    LINK_HELLO = 0x01,
    LINK_SINK = 0x02,
    LINK_SOURCE = 0x03,
    LINK_ECHO = 0x04,
    LINK_HELLO_R = 0x81,
    LINK_SINK_ACK = 0x82,
    LINK_DATA = 0x83,
    LINK_ECHO_R = 0x84,
    LINK_SOURCE_DONE = 0x85,
    LINK_ERROR = 0xFF,
};

#define LINK_ECHO_MAX 65536
#define LINK_CHUNK_MAX 16384

struct link_io {
    /* Write all n bytes; return 0, or -1 if the link is gone. May block. */
    int (*write)(void *ctx, const void *buf, size_t n);
    void *ctx;
    const char *hello; /* device description returned in HELLO_R */
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
};

void link_init(struct link *l, const struct link_io *io);
void link_feed(struct link *l, const uint8_t *data, size_t n);

#endif
