#include "codec/cinepak.h"

#include <stdlib.h>
#include <string.h>

static uint32_t rd16be(const uint8_t *p)
{
    return (uint32_t)(p[0] << 8 | p[1]);
}

static uint32_t rd24be(const uint8_t *p)
{
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

static uint32_t rd32be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

bool ft_cinepak_init(ft_cinepak *c, uint32_t w, uint32_t h)
{
    memset(c, 0, sizeof *c);
    c->w = w;
    c->h = h;
    c->rgb = calloc((size_t)w * h, 3);
    if (!c->rgb) {
        ft_set_error("out of memory");
        return false;
    }
    return true;
}

void ft_cinepak_free(ft_cinepak *c)
{
    free(c->rgb);
    c->rgb = NULL;
}

/*
 * A codebook chunk: 0x20-0x27, bit 0 = partial update (a 32-bit mask per 32 entries says which
 * follow), bit 2 = luma only (4 bytes an entry instead of 6).
 */
static void decode_codebook(ft_cinepak_entry *book, int id, const uint8_t *p, const uint8_t *end)
{
    int n = (id & 0x04) ? 4 : 6;
    uint32_t flag = 0, mask = 0;
    for (int i = 0; i < 256; i++) {
        if ((id & 0x01) && !(mask >>= 1)) {
            if (end - p < 4)
                return;
            flag = rd32be(p);
            p += 4;
            mask = 0x80000000u;
        }
        if ((id & 0x01) && !(flag & mask))
            continue;
        if (end - p < n)
            return;
        uint8_t *e = book[i];
        if (n == 4) {
            for (int k = 0; k < 4; k++)
                e[k * 3] = e[k * 3 + 1] = e[k * 3 + 2] = p[k];
        } else {
            int u = (int8_t)p[4], v = (int8_t)p[5];
            for (int k = 0; k < 4; k++) {
                int y = p[k];
                e[k * 3] = clamp8(y + v * 2);
                e[k * 3 + 1] = clamp8(y - u / 2 - v);
                e[k * 3 + 2] = clamp8(y + u * 2);
            }
        }
        p += n;
    }
}

/*
 * Blocks are written bottom row first: one straddling the bottom edge has its lower rows aimed at
 * the last row, which its top rows then overwrite with the right data (as the reference decoder).
 */

/* One 4x4 block from a V1 entry: each pixel of the 2x2 entry covers 2x2 pixels. */
static void put_v1(uint8_t *rows[4], const uint8_t *e)
{
    for (int r = 3; r >= 0; r--) {
        const uint8_t *src = e + (r / 2) * 6;
        for (int x = 0; x < 4; x++)
            memcpy(rows[r] + x * 3, src + (x / 2) * 3, 3);
    }
}

/* One 4x4 block from four V4 entries, one 2x2 per quarter (top left, top right, bottom left, ...). */
static void put_v4(uint8_t *rows[4], const uint8_t *e[4])
{
    for (int r = 3; r >= 0; r--) {
        const uint8_t *left = e[(r / 2) * 2] + (r % 2) * 6, *right = e[(r / 2) * 2 + 1] + (r % 2) * 6;
        memcpy(rows[r], left, 6);
        memcpy(rows[r] + 6, right, 6);
    }
}

/*
 * The block list: 0x30 = every block coded, with a flag per block (V4 or V1); 0x31 = a flag per
 * block says whether it's coded at all, then a second one V4 or V1; 0x32 = every block V1.
 */
static bool decode_blocks(ft_cinepak *c, int strip, int id, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2,
                          const uint8_t *p, const uint8_t *end)
{
    uint32_t flag = 0, mask = 0;
    size_t stride = (size_t)c->w * 3;
    for (uint32_t y = y1; y < y2; y += 4) {
        uint8_t *rows[4]; /* past the bottom edge, a row aliases the one above */
        rows[0] = c->rgb + y * stride + (size_t)x1 * 3;
        for (int r = 1; r < 4; r++)
            rows[r] = c->h - y > (uint32_t)r ? rows[r - 1] + stride : rows[r - 1];
        for (uint32_t x = x1; x < x2; x += 4) {
            if ((id & 0x01) && !(mask >>= 1)) {
                if (end - p < 4)
                    return false;
                flag = rd32be(p);
                p += 4;
                mask = 0x80000000u;
            }
            if (!(id & 0x01) || (flag & mask)) {
                if (!(id & 0x02) && !(mask >>= 1)) {
                    if (end - p < 4)
                        return false;
                    flag = rd32be(p);
                    p += 4;
                    mask = 0x80000000u;
                }
                if ((id & 0x02) || !(flag & mask)) {
                    if (end - p < 1)
                        return false;
                    put_v1(rows, c->v1[strip][*p++]);
                } else {
                    if (end - p < 4)
                        return false;
                    const uint8_t *e[4] = { c->v4[strip][p[0]], c->v4[strip][p[1]], c->v4[strip][p[2]],
                                            c->v4[strip][p[3]] };
                    p += 4;
                    put_v4(rows, e);
                }
            }
            for (int r = 0; r < 4; r++)
                rows[r] += 12;
        }
    }
    return true;
}

bool ft_cinepak_decode(ft_cinepak *c, const uint8_t *data, size_t size)
{
    if (size < 10) {
        ft_set_error("cinepak: short frame");
        return false;
    }
    const uint8_t *end = data + size;
    int frame_flags = data[0];
    uint32_t strips = rd16be(data + 8);
    if (strips > FT_CINEPAK_STRIPS)
        strips = FT_CINEPAK_STRIPS;
    const uint8_t *p = data + 10;
    uint32_t y0 = 0;
    for (uint32_t i = 0; i < strips; i++) {
        if (end - p < 12) {
            ft_set_error("cinepak: short strip header");
            return false;
        }
        /* a zero top means "below the previous strip", with the bottom as a height */
        uint32_t y1 = rd16be(p + 4), x1 = rd16be(p + 6), y2 = rd16be(p + 8), x2 = rd16be(p + 10);
        if (y1 == 0) {
            y1 = y0;
            y2 = y0 + y2;
        }
        int32_t strip_size = (int32_t)rd24be(p + 1) - 12;
        if (strip_size < 0) {
            ft_set_error("cinepak: bad strip size");
            return false;
        }
        p += 12;
        if (strip_size > end - p)
            strip_size = (int32_t)(end - p);
        /* without frame flag 1, a strip starts from the previous strip's codebooks */
        if (i > 0 && !(frame_flags & 0x01)) {
            memcpy(c->v4[i], c->v4[i - 1], sizeof c->v4[i]);
            memcpy(c->v1[i], c->v1[i - 1], sizeof c->v1[i]);
        }
        if (x2 > c->w || y2 > c->h || x1 >= x2 || y1 >= y2) {
            ft_set_error("cinepak: strip outside the picture");
            return false;
        }

        const uint8_t *s = p, *s_end = p + strip_size;
        bool done = false;
        while (!done && s_end - s >= 4) {
            int id = s[0];
            int32_t chunk = (int32_t)rd24be(s + 1) - 4;
            if (chunk < 0) {
                ft_set_error("cinepak: bad chunk size");
                return false;
            }
            s += 4;
            if (chunk > s_end - s)
                chunk = (int32_t)(s_end - s);
            switch (id) {
            case 0x20: case 0x21: case 0x24: case 0x25:
                decode_codebook(c->v4[i], id, s, s + chunk);
                break;
            case 0x22: case 0x23: case 0x26: case 0x27:
                decode_codebook(c->v1[i], id, s, s + chunk);
                break;
            case 0x30: case 0x31: case 0x32:
                if (!decode_blocks(c, (int)i, id, x1, y1, x2, y2, s, s + chunk)) {
                    ft_set_error("cinepak: short block list");
                    return false;
                }
                done = true;
                break;
            default:
                break;
            }
            s += chunk;
        }
        p += strip_size;
        y0 = y2;
    }
    return true;
}
