#include "codec/inflate.h"

#include "core/ft_common.h"

#include <string.h>

/*
 * Canonical Huffman decoding with a 10-bit lookup table for the short codes and a
 * bit-by-bit walk over the code-length counts for the rest.
 */
#define MAX_BITS 15
#define LUT_BITS 10
#define MAX_SYMBOLS 320

typedef struct huff {
    uint16_t count[MAX_BITS + 1];  /* codes of each length */
    uint16_t symbol[MAX_SYMBOLS];  /* symbols ordered by code */
    uint16_t lut[1 << LUT_BITS];   /* (length << 9) | symbol for codes <= LUT_BITS; 0 = longer */
} huff;

typedef struct state {
    const uint8_t *in;
    size_t in_len, in_pos;
    uint64_t bits;
    int nbits;
    uint8_t *out;
    size_t out_cap, out_pos;
} state;

static void refill(state *s)
{
    while (s->nbits <= 56) {
        uint64_t b = s->in_pos < s->in_len ? s->in[s->in_pos] : 0; /* past the end reads 0 */
        s->in_pos++;
        s->bits |= b << s->nbits;
        s->nbits += 8;
    }
}

/* Bits consumed beyond the input mean a truncated stream. */
static bool overrun(const state *s)
{
    return s->in_pos * 8 - (size_t)s->nbits > s->in_len * 8;
}

static uint32_t getbits(state *s, int n)
{
    if (s->nbits < n)
        refill(s);
    uint32_t v = (uint32_t)(s->bits & ((1ull << n) - 1));
    s->bits >>= n;
    s->nbits -= n;
    return v;
}

static uint32_t reverse_bits(uint32_t code, int len)
{
    uint32_t r = 0;
    for (int i = 0; i < len; i++, code >>= 1)
        r = (r << 1) | (code & 1);
    return r;
}

/* Builds the decoder for `n` code lengths; incomplete codes are allowed, oversubscribed ones not. */
static bool build(huff *h, const uint8_t *lengths, int n)
{
    memset(h->count, 0, sizeof h->count);
    memset(h->lut, 0, sizeof h->lut);
    for (int i = 0; i < n; i++)
        h->count[lengths[i]]++;
    int left = 1;
    for (int len = 1; len <= MAX_BITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return false;
    }
    uint16_t offs[MAX_BITS + 2];
    offs[1] = 0;
    for (int len = 1; len <= MAX_BITS; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    /* canonical codes in symbol order: consecutive within a length, doubled between lengths */
    uint32_t code = 0;
    int k = 0;
    for (int len = 1; len <= MAX_BITS; len++, code <<= 1)
        for (int j = 0; j < h->count[len]; j++, code++, k++) {
            if (len > LUT_BITS)
                continue;
            uint16_t e = (uint16_t)((len << 9) | h->symbol[k]);
            for (uint32_t r = reverse_bits(code, len); r < (1u << LUT_BITS); r += 1u << len)
                h->lut[r] = e;
        }
    return true;
}

static int decode(state *s, const huff *h)
{
    if (s->nbits < MAX_BITS)
        refill(s);
    uint16_t e = h->lut[s->bits & ((1u << LUT_BITS) - 1)];
    if (e) {
        int len = e >> 9;
        s->bits >>= len;
        s->nbits -= len;
        return e & 511;
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAX_BITS; len++) {
        code |= (int)((s->bits >> (len - 1)) & 1);
        int count = h->count[len];
        if (code - count < first) {
            s->bits >>= len;
            s->nbits -= len;
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t len_base[29] = { 3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static bool codes(state *s, const huff *lit, const huff *dist)
{
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 0 || overrun(s)) {
            ft_set_error("inflate: bad literal/length code");
            return false;
        }
        if (sym < 256) {
            if (s->out_pos >= s->out_cap) {
                ft_set_error("inflate: output too large");
                return false;
            }
            s->out[s->out_pos++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256)
            return true;
        sym -= 257;
        if (sym >= 29) {
            ft_set_error("inflate: bad length symbol");
            return false;
        }
        size_t len = len_base[sym] + getbits(s, len_extra[sym]);
        int d = decode(s, dist);
        if (d < 0 || d >= 30) {
            ft_set_error("inflate: bad distance code");
            return false;
        }
        size_t back = dist_base[d] + getbits(s, dist_extra[d]);
        if (back > s->out_pos || len > s->out_cap - s->out_pos) {
            ft_set_error("inflate: distance or length out of range");
            return false;
        }
        uint8_t *dst = s->out + s->out_pos;
        const uint8_t *src = dst - back;
        for (size_t i = 0; i < len; i++) /* may overlap */
            dst[i] = src[i];
        s->out_pos += len;
    }
}

static bool stored(state *s)
{
    getbits(s, s->nbits & 7); /* to a byte boundary */
    /* return whole buffered bytes to the input, then read LEN / NLEN directly */
    s->in_pos -= (size_t)(s->nbits / 8);
    s->bits = 0;
    s->nbits = 0;
    if (s->in_pos + 4 > s->in_len) {
        ft_set_error("inflate: truncated stored block");
        return false;
    }
    uint16_t len = ft_rd16(s->in + s->in_pos), nlen = ft_rd16(s->in + s->in_pos + 2);
    s->in_pos += 4;
    if ((len ^ nlen) != 0xFFFF || s->in_pos + len > s->in_len || len > s->out_cap - s->out_pos) { /* NLEN = ~LEN */
        ft_set_error("inflate: bad stored block");
        return false;
    }
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len;
    s->out_pos += len;
    return true;
}

static bool fixed(state *s)
{
    static huff lit, dist;
    static bool ready;
    if (!ready) {
        uint8_t l[288];
        int i = 0;
        for (; i < 144; i++)
            l[i] = 8;
        for (; i < 256; i++)
            l[i] = 9;
        for (; i < 280; i++)
            l[i] = 7;
        for (; i < 288; i++)
            l[i] = 8;
        build(&lit, l, 288);
        for (i = 0; i < 30; i++)
            l[i] = 5;
        build(&dist, l, 30);
        ready = true;
    }
    return codes(s, &lit, &dist);
}

static bool dynamic(state *s)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nlen = (int)getbits(s, 5) + 257, ndist = (int)getbits(s, 5) + 1, ncode = (int)getbits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) {
        ft_set_error("inflate: bad dynamic header");
        return false;
    }
    uint8_t lengths[320] = { 0 };
    for (int i = 0; i < ncode; i++)
        lengths[order[i]] = (uint8_t)getbits(s, 3);
    huff lencode, lit, dist;
    if (!build(&lencode, lengths, 19)) {
        ft_set_error("inflate: bad code-length code");
        return false;
    }
    int i = 0;
    while (i < nlen + ndist) {
        int sym = decode(s, &lencode);
        if (sym < 0 || overrun(s)) {
            ft_set_error("inflate: bad code length");
            return false;
        }
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        uint8_t value = 0;
        int repeat;
        if (sym == 16) {
            if (i == 0) {
                ft_set_error("inflate: repeat with no previous length");
                return false;
            }
            value = lengths[i - 1];
            repeat = 3 + (int)getbits(s, 2);
        } else if (sym == 17) {
            repeat = 3 + (int)getbits(s, 3);
        } else {
            repeat = 11 + (int)getbits(s, 7);
        }
        if (i + repeat > nlen + ndist) {
            ft_set_error("inflate: too many code lengths");
            return false;
        }
        while (repeat--)
            lengths[i++] = value;
    }
    if (lengths[256] == 0 || !build(&lit, lengths, nlen) || !build(&dist, lengths + nlen, ndist)) {
        ft_set_error("inflate: bad literal or distance lengths");
        return false;
    }
    return codes(s, &lit, &dist);
}

bool ft_inflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len)
{
    state s = { .in = in, .in_len = in_len, .out = out, .out_cap = out_cap };
    bool last;
    do {
        last = getbits(&s, 1);
        uint32_t type = getbits(&s, 2);
        bool ok = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : false;
        if (type == 3)
            ft_set_error("inflate: bad block type");
        if (!ok)
            return false;
        if (overrun(&s)) {
            ft_set_error("inflate: truncated input");
            return false;
        }
    } while (!last);
    *out_len = s.out_pos;
    return true;
}
