/*
 * Anti-aliased bitmap font reader (.ipfn). See docs/font-format.md in the iPodOS repo.
 *
 * Fonts are loaded whole into RAM (17-39 KB each) and validated once; after ipfn_open()
 * every glyph index, bitmap offset and range is in bounds, so drawing does no checking.
 *
 * No allocation, no I/O. Little-endian hosts only.
 */
#ifndef IPFN_H
#define IPFN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum ipfn_err {
    IPFN_OK = 0,
    IPFN_E_ALIGN,
    IPFN_E_SHORT,
    IPFN_E_MAGIC,
    IPFN_E_VERSION,
    IPFN_E_HEADER,
    IPFN_E_CRC,
    IPFN_E_TABLES,
    IPFN_E_REF
};

typedef struct {
    const uint8_t *ranges;
    const uint8_t *glyphs;
    const uint8_t *bitmaps;
    uint32_t bitmap_size;
    uint16_t n_ranges;
    uint16_t n_glyphs;
    uint16_t px;
    uint16_t ascent;
    uint16_t descent;
    uint16_t line_height;
    int16_t tracking;
} ipfn_font;

typedef struct {
    const uint8_t *bitmap; /* w*h coverage bytes, NULL when blank */
    uint8_t w, h;
    int8_t left, top;      /* top is above the baseline, positive up */
    uint8_t advance;
} ipfn_glyph;

int ipfn_open(ipfn_font *f, const void *buf, size_t len);
const char *ipfn_strerror(int err);

/* Decode one UTF-8 codepoint; advances *s. Invalid bytes yield U+FFFD. */
uint32_t ipfn_utf8_next(const char **s);

void ipfn_get_glyph(const ipfn_font *f, uint32_t cp, ipfn_glyph *g);

/* Width in pixels of a UTF-8 string, tracking included. */
int ipfn_width(const ipfn_font *f, const char *text);

/* Width of the first n bytes. */
int ipfn_width_n(const ipfn_font *f, const char *text, size_t n);

/* Longest prefix (in bytes) that fits in max_w pixels. */
size_t ipfn_fit(const ipfn_font *f, const char *text, int max_w);

#endif
