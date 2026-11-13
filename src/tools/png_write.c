#include "tools/png_write.h"

#include "core/ft_common.h"

#include <stdlib.h>
#include <string.h>

/* ---- CRC32 / Adler32 ---------------------------------------------------- */

static uint32_t crc_table[256];

static void crc_init(void)
{
    if (crc_table[1])
        return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        size_t chunk = n < 5552 ? n : 5552; /* largest run without overflow before the modulo */
        n -= chunk;
        while (chunk--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

static void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* ---- deflate (fixed Huffman, single block) ------------------------------ */

typedef struct bitw {
    uint8_t *buf;
    size_t len;
    uint32_t bits;
    int nbits;
} bitw;

static void put_bits(bitw *w, uint32_t v, int n)
{
    w->bits |= v << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        w->buf[w->len++] = (uint8_t)w->bits;
        w->bits >>= 8;
        w->nbits -= 8;
    }
}

/* Huffman codes go out MSB first. */
static void put_code(bitw *w, uint32_t code, int n)
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++)
        r |= ((code >> i) & 1) << (n - 1 - i);
    put_bits(w, r, n);
}

static void put_litlen(bitw *w, unsigned sym)
{
    if (sym < 144)
        put_code(w, 0x30 + sym, 8);
    else if (sym < 256)
        put_code(w, 0x190 + (sym - 144), 9);
    else if (sym < 280)
        put_code(w, sym - 256, 7);
    else
        put_code(w, 0xC0 + (sym - 280), 8);
}

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void put_match(bitw *w, unsigned len, unsigned dist)
{
    int lc = 28;
    while (len_base[lc] > len)
        lc--;
    put_litlen(w, 257 + (unsigned)lc);
    put_bits(w, len - len_base[lc], len_extra[lc]);
    int dc = 29;
    while (dist_base[dc] > dist)
        dc--;
    put_code(w, (uint32_t)dc, 5);
    put_bits(w, dist - dist_base[dc], dist_extra[dc]);
}

#define WINDOW 32768
#define HASH_BITS 15
#define MAX_CHAIN 32
#define MIN_MATCH 3
#define MAX_MATCH 258

static unsigned hash3(const uint8_t *p)
{
    return (((unsigned)p[0] << 10) ^ ((unsigned)p[1] << 5) ^ p[2]) & ((1u << HASH_BITS) - 1);
}

/* Compresses `src` into a fresh buffer; returns its length or 0 on allocation failure. */
static size_t deflate_fixed(const uint8_t *src, size_t n, uint8_t **out)
{
    int32_t *head = malloc(sizeof(int32_t) << HASH_BITS);
    int32_t *prev = malloc(sizeof(int32_t) * WINDOW);
    bitw w = { malloc(n + n / 8 + 64), 0, 0, 0 }; /* fixed codes cost at most 9 bits per byte */
    if (!head || !prev || !w.buf) {
        free(head);
        free(prev);
        free(w.buf);
        return 0;
    }
    memset(head, 0xFF, sizeof(int32_t) << HASH_BITS);

    put_bits(&w, 1, 1); /* BFINAL */
    put_bits(&w, 1, 2); /* BTYPE = fixed Huffman */
    size_t i = 0;
    while (i < n) {
        unsigned best = 0, best_dist = 0;
        if (n - i >= MIN_MATCH) {
            size_t limit = n - i < MAX_MATCH ? n - i : MAX_MATCH;
            int32_t cand = head[hash3(src + i)];
            for (int chain = 0; cand >= 0 && (size_t)cand < i && i - (size_t)cand <= WINDOW && chain < MAX_CHAIN;
                 chain++) {
                const uint8_t *a = src + cand, *b = src + i;
                unsigned l = 0;
                while (l < limit && a[l] == b[l])
                    l++;
                if (l > best) {
                    best = l;
                    best_dist = (unsigned)(i - (size_t)cand);
                    if (l == limit)
                        break;
                }
                cand = prev[cand & (WINDOW - 1)];
            }
        }
        size_t step = best >= MIN_MATCH ? best : 1;
        if (best >= MIN_MATCH)
            put_match(&w, best, best_dist);
        else
            put_litlen(&w, src[i]);
        for (size_t k = 0; k < step; k++, i++) {
            if (n - i >= MIN_MATCH) {
                unsigned h = hash3(src + i);
                prev[i & (WINDOW - 1)] = head[h];
                head[h] = (int32_t)i;
            }
        }
    }
    put_litlen(&w, 256);
    if (w.nbits)
        put_bits(&w, 0, 8 - w.nbits);
    free(head);
    free(prev);
    *out = w.buf;
    return w.len;
}

/* ---- PNG ----------------------------------------------------------------- */

static bool write_chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
    uint8_t hdr[8];
    put32be(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    uint32_t crc = crc_update(0xFFFFFFFFu, hdr + 4, 4);
    crc = crc_update(crc, data, len) ^ 0xFFFFFFFFu;
    uint8_t tail[4];
    put32be(tail, crc);
    return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(tail, 1, 4, f) == 4;
}

static uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

bool ft_png_write(const char *path, const ft_image *img)
{
    crc_init();
    size_t stride = (size_t)img->w * 4, row = stride + 1;
    size_t raw_len = row * img->h;
    uint8_t *rgba = malloc(stride * img->h), *raw = malloc(raw_len), *z = NULL;
    if (!rgba || !raw) {
        free(rgba);
        free(raw);
        ft_set_error("out of memory writing %s", path);
        return false;
    }
    for (size_t i = 0; i < (size_t)img->w * img->h; i++) {
        rgba[i * 4 + 0] = img->bgra[i * 4 + 2];
        rgba[i * 4 + 1] = img->bgra[i * 4 + 1];
        rgba[i * 4 + 2] = img->bgra[i * 4 + 0];
        rgba[i * 4 + 3] = img->bgra[i * 4 + 3];
    }
    for (uint32_t y = 0; y < img->h; y++) {
        const uint8_t *cur = rgba + y * stride, *up = y ? cur - stride : NULL;
        uint8_t *dst = raw + y * row;
        dst[0] = 4; /* Paeth */
        for (size_t x = 0; x < stride; x++) {
            int a = x >= 4 ? cur[x - 4] : 0, b = up ? up[x] : 0, c = (up && x >= 4) ? up[x - 4] : 0;
            dst[1 + x] = (uint8_t)(cur[x] - paeth(a, b, c));
        }
    }
    free(rgba);

    uint8_t *def = NULL;
    size_t def_len = deflate_fixed(raw, raw_len, &def);
    if (def_len)
        z = malloc(def_len + 6);
    if (!z) {
        free(raw);
        free(def);
        ft_set_error("out of memory writing %s", path);
        return false;
    }
    z[0] = 0x78, z[1] = 0x01;
    memcpy(z + 2, def, def_len);
    put32be(z + 2 + def_len, adler32(raw, raw_len));
    free(def);
    free(raw);

    uint8_t ihdr[13];
    put32be(ihdr, img->w);
    put32be(ihdr + 4, img->h);
    ihdr[8] = 8, ihdr[9] = 6, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;

    FILE *f = ft_fopen(path, "wb");
    bool ok = f && fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 && write_chunk(f, "IHDR", ihdr, 13) &&
              write_chunk(f, "IDAT", z, def_len + 6) && write_chunk(f, "IEND", NULL, 0);
    if (f)
        ok = (fclose(f) == 0) && ok;
    if (!ok)
        ft_set_error("cannot write %s", path);
    free(z);
    return ok;
}
