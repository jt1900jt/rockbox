#include "link_proto.h"

#include <string.h>

#include "ipdb.h" /* ipdb_crc32 */

static uint8_t pattern[LINK_CHUNK_MAX];
static int pattern_ready;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int send_frame_crc(struct link *l, uint8_t type, uint8_t flags, uint32_t seq,
                          const void *payload, uint32_t n, uint32_t crc)
{
    uint8_t h[LINK_HDR];
    h[0] = LINK_MAGIC0;
    h[1] = LINK_MAGIC1;
    h[2] = type;
    h[3] = flags;
    put32(h + 4, seq);
    put32(h + 8, n);
    put32(h + 12, (flags & LINK_F_CRC) ? crc : 0);
    if (l->io.write(l->io.ctx, h, sizeof h) < 0)
        return -1;
    if (n && l->io.write(l->io.ctx, payload, n) < 0)
        return -1;
    l->st.tx_bytes += sizeof h + n;
    return 0;
}

static int send_frame(struct link *l, uint8_t type, uint8_t flags, uint32_t seq, const void *payload, uint32_t n)
{
    uint32_t crc = (flags & LINK_F_CRC) ? ipdb_crc32(0, payload, n) : 0;
    return send_frame_crc(l, type, flags, seq, payload, n, crc);
}

static void send_error(struct link *l, uint32_t seq, const char *msg)
{
    l->st.errors++;
    send_frame(l, LINK_ERROR, 0, seq, msg, (uint32_t)strlen(msg));
}

void link_init(struct link *l, const struct link_io *io)
{
    memset(l, 0, sizeof *l);
    l->io = *io;
    if (!pattern_ready) {
        for (size_t i = 0; i < sizeof pattern; i++)
            pattern[i] = (uint8_t)(i * 31 + 7);
        pattern_ready = 1;
    }
}

static void run_source(struct link *l)
{
    uint32_t total = get32(l->small), chunk = get32(l->small + 4), fl = get32(l->small + 8) & LINK_F_CRC;
    if (chunk == 0 || chunk > LINK_CHUNK_MAX)
        chunk = LINK_CHUNK_MAX;
    uint32_t full_crc = fl ? ipdb_crc32(0, pattern, chunk) : 0;
    uint32_t sent = 0, seq = 0;
    while (sent < total) {
        uint32_t n = total - sent < chunk ? total - sent : chunk;
        uint32_t c = !fl ? 0 : n == chunk ? full_crc : ipdb_crc32(0, pattern, n);
        if (send_frame_crc(l, LINK_DATA, (uint8_t)fl, seq++, pattern, n, c) < 0)
            return;
        sent += n;
    }
    uint8_t done[4];
    put32(done, total);
    send_frame(l, LINK_SOURCE_DONE, 0, l->seq, done, sizeof done);
}

static void begin_payload(struct link *l)
{
    l->in_payload = 1;
    l->remaining = l->len;
    l->crc_run = 0;
    l->small_have = 0;
}

static void consume(struct link *l, const uint8_t *p, size_t n)
{
    if (l->flags & LINK_F_CRC)
        l->crc_run = ipdb_crc32(l->crc_run, p, n);
    uint32_t pos = l->len - l->remaining;
    switch (l->type) {
    case LINK_ECHO:
        if (l->len <= LINK_ECHO_MAX)
            memcpy(l->echo + pos, p, n);
        break;
    case LINK_SOURCE:
        if (pos < sizeof l->small) {
            size_t k = n < sizeof l->small - pos ? n : sizeof l->small - pos;
            memcpy(l->small + pos, p, k);
            l->small_have = pos + k;
        }
        break;
    default:
        break;
    }
}

static void finish(struct link *l)
{
    uint8_t ack[8];
    l->in_payload = 0;
    l->hdr_have = 0;
    l->st.frames++;
    int crc_ok = !(l->flags & LINK_F_CRC) || l->crc_run == l->crc_expect;
    if (!crc_ok)
        l->st.crc_errors++;

    switch (l->type) {
    case LINK_HELLO: {
        char msg[96];
        size_t k = 0;
        const char *pre = "ipodos-link/1 ";
        while (*pre && k < sizeof msg - 1)
            msg[k++] = *pre++;
        for (const char *h = l->io.hello ? l->io.hello : ""; *h && k < sizeof msg - 1; h++)
            msg[k++] = *h;
        send_frame(l, LINK_HELLO_R, 0, l->seq, msg, (uint32_t)k);
        break;
    }
    case LINK_SINK:
        put32(ack, l->len);
        put32(ack + 4, (uint32_t)crc_ok);
        send_frame(l, LINK_SINK_ACK, 0, l->seq, ack, sizeof ack);
        break;
    case LINK_ECHO:
        if (l->len > LINK_ECHO_MAX)
            send_error(l, l->seq, "echo payload too large");
        else if (!crc_ok)
            send_error(l, l->seq, "crc mismatch");
        else
            send_frame(l, LINK_ECHO_R, l->flags & LINK_F_CRC, l->seq, l->echo, l->len);
        break;
    case LINK_SOURCE:
        if (l->len != 12 || l->small_have != 12)
            send_error(l, l->seq, "bad source request");
        else if (!crc_ok)
            send_error(l, l->seq, "crc mismatch");
        else
            run_source(l);
        break;
    default:
        send_error(l, l->seq, "unknown frame type");
        break;
    }
}

void link_feed(struct link *l, const uint8_t *data, size_t n)
{
    l->st.rx_bytes += n;
    while (n) {
        if (!l->in_payload) {
            size_t take = LINK_HDR - l->hdr_have;
            if (take > n)
                take = n;
            memcpy(l->hdr + l->hdr_have, data, take);
            l->hdr_have += take;
            data += take;
            n -= take;
            if (l->hdr_have < LINK_HDR)
                break;
            if (l->hdr[0] != LINK_MAGIC0 || l->hdr[1] != LINK_MAGIC1) {
                memmove(l->hdr, l->hdr + 1, LINK_HDR - 1);
                l->hdr_have = LINK_HDR - 1;
                l->st.resyncs++;
                continue;
            }
            l->type = l->hdr[2];
            l->flags = l->hdr[3];
            l->seq = get32(l->hdr + 4);
            l->len = get32(l->hdr + 8);
            l->crc_expect = get32(l->hdr + 12);
            begin_payload(l);
            if (l->remaining == 0)
                finish(l);
        } else {
            size_t take = l->remaining < n ? l->remaining : n;
            consume(l, data, take);
            l->remaining -= (uint32_t)take;
            data += take;
            n -= take;
            if (l->remaining == 0)
                finish(l);
        }
    }
}
