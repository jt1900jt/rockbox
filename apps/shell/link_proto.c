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

static int send_ok(struct link *l)
{
    return send_frame(l, LINK_OK, 0, l->seq, NULL, 0);
}

/* Paths arrive as the frame payload. Reject anything that could escape the device root
 * or overflow the buffer; the companion is trusted, but a corrupted frame is not.
 *
 * Only a whole component of ".." escapes the root. Rejecting the substring anywhere
 * also refused ordinary filenames: "R.O.D..m4a" is legitimate. */
static bool path_ok(const struct link *l)
{
    if (l->path_have == 0 || l->path_have >= LINK_PATH_MAX)
        return false;
    if (l->path[0] != '/')
        return false;

    const char *p = l->path;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t n = (size_t)(p - start);
        if (n == 2 && start[0] == '.' && start[1] == '.')
            return false;
        while (*p == '/')
            p++;
    }
    return true;
}

/* The listing buffer is static, not a local: the device runs this on its UI thread,
 * whose stack is a few kilobytes. A 16 KB local overflowed it and took the player
 * down partway through indexing a real library. One listing runs at a time. */
#define LIST_BUF 4096
static uint8_t list_buf[LIST_BUF];

struct emit_state {
    struct link *l;
    size_t used;
    int failed;
};

static void emit_flush(struct emit_state *e)
{
    if (e->used && send_frame(e->l, LINK_LIST_R, 0, e->l->seq, list_buf, (uint32_t)e->used) < 0)
        e->failed = 1;
    e->used = 0;
}

static void emit_entry(void *ctx, const char *name, int kind, uint32_t size, uint32_t mtime)
{
    struct emit_state *e = ctx;
    size_t n = strlen(name);
    if (e->failed || n > 255)
        return;
    size_t need = 1 + 4 + 4 + 2 + n;
    if (need > LIST_BUF)
        return; /* absurd name: skip rather than overflow */
    if (e->used + need > LIST_BUF)
        emit_flush(e);
    uint8_t *p = list_buf + e->used;
    *p++ = (uint8_t)kind;
    put32(p, size); p += 4;
    put32(p, mtime); p += 4;
    p[0] = (uint8_t)(n & 0xFF); p[1] = (uint8_t)(n >> 8); p += 2;
    memcpy(p, name, n);
    e->used += need;
}

static int get_send(void *ctx, const void *buf, size_t n)
{
    struct link *l = ctx;
    return send_frame(l, LINK_DATA, 0, l->seq, buf, (uint32_t)n);
}

static void begin_payload(struct link *l)
{
    l->in_payload = 1;
    l->remaining = l->len;
    l->crc_run = 0;
    l->small_have = 0;
    l->path_have = 0;
    l->chunk_have = 0;
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

    case LINK_STAT:
    case LINK_LIST:
    case LINK_MKDIR:
    case LINK_GET:
    case LINK_DELETE:
        if (l->path_have + n < LINK_PATH_MAX) {
            memcpy(l->path + l->path_have, p, n);
            l->path_have += n;
            l->path[l->path_have] = 0;
        } else {
            l->path_have = LINK_PATH_MAX; /* marks overflow */
        }
        break;

    case LINK_PUT_BEGIN:
        /* header is 9 bytes, then the path */
        if (pos < 9) {
            size_t k = n < 9 - pos ? n : 9 - pos;
            memcpy(l->small + pos, p, k);
            l->small_have = pos + k;
            p += k;
            n -= k;
        }
        if (n && l->path_have + n < LINK_PATH_MAX) {
            memcpy(l->path + l->path_have, p, n);
            l->path_have += n;
            l->path[l->path_have] = 0;
        }
        break;

    case LINK_PUT_DATA:
        /* Streamed straight to storage rather than buffered whole: files are larger
         * than anything we can hold in RAM. */
        if (l->put_open && !l->put_bad) {
            if (l->put_flags & LINK_PUT_F_CRC)
                l->put_crc = ipdb_crc32(l->put_crc, p, n);
            if (l->io.fs && l->io.fs->put_data(l->io.fs->ctx, p, n) < 0)
                l->put_bad = true;
            else
                l->put_written += (uint32_t)n;
        }
        break;

    default:
        break;
    }
}

/* File operations. Each one answers with OK, a typed reply, or ERROR; the companion
 * treats any ERROR as fatal for that file and carries on with the next. */
static void handle_fs(struct link *l, int crc_ok)
{
    const struct link_fs *fs = l->io.fs;
    uint8_t reply[24];

    if (!fs) {
        send_error(l, l->seq, "no filesystem on this link");
        return;
    }
    if (!crc_ok) {
        if (l->type == LINK_PUT_DATA)
            l->put_bad = true;
        send_error(l, l->seq, "crc mismatch");
        return;
    }

    switch (l->type) {
    case LINK_STAT: {
        if (!path_ok(l)) {
            send_error(l, l->seq, "bad path");
            return;
        }
        int kind = 0;
        uint32_t size = 0, mtime = 0;
        if (fs->stat(fs->ctx, l->path, &kind, &size, &mtime) < 0)
            kind = 0;
        reply[0] = (uint8_t)kind;
        put32(reply + 1, size);
        put32(reply + 5, mtime);
        send_frame(l, LINK_STAT_R, 0, l->seq, reply, 9);
        break;
    }

    case LINK_LIST: {
        if (!path_ok(l)) {
            send_error(l, l->seq, "bad path");
            return;
        }
        struct emit_state e = { .l = l, .used = 0, .failed = 0 };
        /* reset the shared buffer in case a previous listing was interrupted */
        if (fs->list(fs->ctx, l->path, emit_entry, &e) < 0) {
            send_error(l, l->seq, "cannot list");
            return;
        }
        emit_flush(&e);
        send_ok(l);
        break;
    }

    case LINK_MKDIR:
        if (!path_ok(l)) {
            send_error(l, l->seq, "bad path");
            return;
        }
        if (fs->mkdir(fs->ctx, l->path) < 0)
            send_error(l, l->seq, "cannot create directory");
        else
            send_ok(l);
        break;

    case LINK_DELETE:
        if (!path_ok(l)) {
            send_error(l, l->seq, "bad path");
            return;
        }
        if (fs->remove(fs->ctx, l->path) < 0)
            send_error(l, l->seq, "cannot delete");
        else
            send_ok(l);
        break;

    case LINK_PUT_BEGIN: {
        if (l->put_open) /* a previous transfer was abandoned */
            fs->put_end(fs->ctx, false);
        l->put_open = false;
        l->put_bad = false;
        l->put_written = 0;
        l->put_crc = 0;
        if (l->small_have < 9 || !path_ok(l)) {
            send_error(l, l->seq, "bad put header");
            return;
        }
        uint32_t size = get32(l->small);
        l->put_expect_crc = get32(l->small + 4);
        l->put_flags = l->small[8];
        if (fs->put_begin(fs->ctx, l->path, size) < 0) {
            send_error(l, l->seq, "cannot open for writing");
            return;
        }
        l->put_open = true;
        send_ok(l);
        break;
    }

    case LINK_PUT_DATA:
        /* The payload was streamed to storage in consume(); nothing to answer unless
         * it failed, and the companion learns that at PUT_END. */
        break;

    case LINK_PUT_END: {
        if (!l->put_open) {
            send_error(l, l->seq, "no transfer in progress");
            return;
        }
        bool ok = !l->put_bad;
        if (ok && (l->put_flags & LINK_PUT_F_CRC) && l->put_crc != l->put_expect_crc)
            ok = false;
        /* Commit by rename only when the whole file arrived intact, so an interrupted
         * sync never leaves a half-written file under its real name. */
        if (fs->put_end(fs->ctx, ok) < 0)
            ok = false;
        l->put_open = false;
        if (!ok) {
            send_error(l, l->seq, "transfer failed");
            return;
        }
        put32(reply, l->put_written);
        send_frame(l, LINK_OK, 0, l->seq, reply, 4);
        break;
    }

    case LINK_GET: {
        if (!path_ok(l)) {
            send_error(l, l->seq, "bad path");
            return;
        }
        int n = fs->get(fs->ctx, l->path, get_send, l);
        if (n < 0) {
            send_error(l, l->seq, "cannot read");
            return;
        }
        put32(reply, (uint32_t)n);
        send_frame(l, LINK_GET_DONE, 0, l->seq, reply, 4);
        break;
    }

    case LINK_FREE: {
        uint64_t freeb = 0, total = 0;
        if (fs->freespace(fs->ctx, &freeb, &total) < 0) {
            send_error(l, l->seq, "cannot stat volume");
            return;
        }
        for (int i = 0; i < 8; i++) {
            reply[i] = (uint8_t)(freeb >> (8 * i));
            reply[8 + i] = (uint8_t)(total >> (8 * i));
        }
        send_frame(l, LINK_FREE_R, 0, l->seq, reply, 16);
        break;
    }

    case LINK_SYNC_DONE:
        if (fs->sync_done)
            fs->sync_done(fs->ctx);
        send_ok(l);
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
    case LINK_STAT:
    case LINK_LIST:
    case LINK_MKDIR:
    case LINK_GET:
    case LINK_DELETE:
    case LINK_PUT_BEGIN:
    case LINK_PUT_DATA:
    case LINK_PUT_END:
    case LINK_FREE:
    case LINK_SYNC_DONE:
        handle_fs(l, crc_ok);
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
