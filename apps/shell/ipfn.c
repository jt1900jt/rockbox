#include "ipfn.h"

#include <string.h>

#include "ipdb.h" /* ipdb_crc32 */

#define IPFN_HEADER 64u
#define RANGE_SIZE  8u
#define GLYPH_SIZE  12u

static uint16_t rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

const char *ipfn_strerror(int err)
{
    static const char *const msgs[] = {
        "ok", "buffer misaligned", "file truncated", "bad magic", "unsupported version",
        "bad header", "checksum mismatch", "tables inconsistent", "reference out of range",
    };
    return (err >= 0 && err < (int)(sizeof msgs / sizeof msgs[0])) ? msgs[err] : "unknown error";
}

int ipfn_open(ipfn_font *f, const void *buf, size_t len)
{
    const uint8_t *b = buf;
    memset(f, 0, sizeof *f);

    if ((uintptr_t)b & 3)
        return IPFN_E_ALIGN;
    if (len < IPFN_HEADER)
        return IPFN_E_SHORT;
    if (memcmp(b, "IPFN", 4) != 0)
        return IPFN_E_MAGIC;
    if (rd16(b + 4) != 1)
        return IPFN_E_VERSION;
    if (rd32(b + 8) != IPFN_HEADER)
        return IPFN_E_HEADER;

    uint32_t file_size = rd32(b + 12);
    if (file_size > len || file_size < IPFN_HEADER)
        return IPFN_E_SHORT;
    if (ipdb_crc32(0, b + IPFN_HEADER, file_size - IPFN_HEADER) != rd32(b + 16))
        return IPFN_E_CRC;

    uint16_t n_glyphs = rd16(b + 28), n_ranges = rd16(b + 30);
    uint32_t ro = rd32(b + 36), go = rd32(b + 40), bo = rd32(b + 44), bs = rd32(b + 48);

    if (n_glyphs == 0)
        return IPFN_E_TABLES;
    if (ro != IPFN_HEADER)
        return IPFN_E_TABLES;
    if (go != ro + (uint32_t)n_ranges * RANGE_SIZE)
        return IPFN_E_TABLES;
    if (bo != go + (uint32_t)n_glyphs * GLYPH_SIZE)
        return IPFN_E_TABLES;
    if (bs > file_size || bo + bs != file_size)
        return IPFN_E_TABLES;

    /* Every glyph's bitmap must lie inside the bitmap section, and every range must
     * index real glyphs; after this, drawing needs no bounds checks. */
    for (uint16_t i = 0; i < n_glyphs; i++) {
        const uint8_t *g = b + go + (uint32_t)i * GLYPH_SIZE;
        uint32_t off = rd32(g);
        uint32_t size = (uint32_t)g[4] * g[5];
        if (size && (off > bs || size > bs - off))
            return IPFN_E_REF;
    }
    uint32_t prev_end = 0;
    for (uint16_t i = 0; i < n_ranges; i++) {
        const uint8_t *r = b + ro + (uint32_t)i * RANGE_SIZE;
        uint32_t first = rd32(r);
        uint32_t count = rd16(r + 4);
        uint32_t start = rd16(r + 6);
        if (count == 0 || first < prev_end)
            return IPFN_E_REF;
        if (first + count < first) /* overflow */
            return IPFN_E_REF;
        prev_end = first + count;
        if (start + count > n_glyphs)
            return IPFN_E_REF;
    }

    f->ranges = b + ro;
    f->glyphs = b + go;
    f->bitmaps = b + bo;
    f->bitmap_size = bs;
    f->n_ranges = n_ranges;
    f->n_glyphs = n_glyphs;
    f->px = rd16(b + 20);
    f->ascent = rd16(b + 22);
    f->descent = rd16(b + 24);
    f->line_height = rd16(b + 26);
    f->tracking = (int16_t)rd16(b + 32);
    return IPFN_OK;
}

uint32_t ipfn_utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = *p++;
    int extra;
    uint32_t min;

    if (c < 0x80) {
        *s = (const char *)p;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        c &= 0x1F; extra = 1; min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        c &= 0x0F; extra = 2; min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        c &= 0x07; extra = 3; min = 0x10000;
    } else {
        *s = (const char *)p;
        return 0xFFFD;
    }

    for (int i = 0; i < extra; i++) {
        if ((*p & 0xC0) != 0x80) {
            *s = (const char *)p;
            return 0xFFFD;
        }
        c = (c << 6) | (*p++ & 0x3F);
    }
    *s = (const char *)p;
    /* reject overlong forms, surrogates and out-of-range values */
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        return 0xFFFD;
    return c;
}

static uint16_t glyph_index(const ipfn_font *f, uint32_t cp)
{
    /* Ranges are sorted, so binary search. */
    uint16_t lo = 0, hi = f->n_ranges;
    while (lo < hi) {
        uint16_t mid = (uint16_t)((lo + hi) / 2);
        const uint8_t *r = f->ranges + (uint32_t)mid * RANGE_SIZE;
        uint32_t first = rd32(r);
        uint32_t count = rd16(r + 4);
        if (cp < first)
            hi = mid;
        else if (cp >= first + count)
            lo = (uint16_t)(mid + 1);
        else
            return (uint16_t)(rd16(r + 6) + (cp - first));
    }
    return 0; /* fallback box */
}

void ipfn_get_glyph(const ipfn_font *f, uint32_t cp, ipfn_glyph *g)
{
    const uint8_t *e = f->glyphs + (uint32_t)glyph_index(f, cp) * GLYPH_SIZE;
    uint32_t off = rd32(e);
    g->w = e[4];
    g->h = e[5];
    g->left = (int8_t)e[6];
    g->top = (int8_t)e[7];
    g->advance = e[8];
    g->bitmap = (g->w && g->h) ? f->bitmaps + off : NULL;
}

int ipfn_width_n(const ipfn_font *f, const char *text, size_t n)
{
    const char *p = text;
    int x = 0;
    bool any = false;
    /* Compare by offset rather than against text + n: a caller-supplied length of
     * SIZE_MAX would overflow the end pointer. */
    while (*p && (size_t)(p - text) < n) {
        const char *before = p;
        uint32_t cp = ipfn_utf8_next(&p);
        if ((size_t)(p - text) > n) { /* sequence straddles the limit */
            p = before;
            break;
        }
        ipfn_glyph g;
        ipfn_get_glyph(f, cp, &g);
        x += g.advance + f->tracking;
        any = true;
    }
    if (any)
        x -= f->tracking;
    return x > 0 ? x : 0;
}

int ipfn_width(const ipfn_font *f, const char *text)
{
    return ipfn_width_n(f, text, (size_t)-1);
}

size_t ipfn_fit(const ipfn_font *f, const char *text, int max_w)
{
    const char *p = text;
    int x = 0;
    size_t fits = 0;
    while (*p) {
        uint32_t cp = ipfn_utf8_next(&p);
        ipfn_glyph g;
        ipfn_get_glyph(f, cp, &g);
        int next = x + (x ? f->tracking : 0) + g.advance;
        if (next > max_w)
            break;
        x = next;
        fits = (size_t)(p - text);
    }
    return fits;
}
