#include "codec/zdf.h"

#include <stdlib.h>
#include <string.h>

#define TAG_FLAG 0x67616c66u /* "flag" */
#define TAG_IMAG 0x67616d69u /* "imag" */

bool ft_zdf_is(const uint8_t *buf, size_t len)
{
    return len >= 4 && (memcmp(buf, "DFS1", 4) == 0 || memcmp(buf, "DFS0", 4) == 0);
}

static uint32_t padded_stride(uint32_t w)
{
    return (w * 3 + 3) & ~3u;
}

bool ft_zdf_open(ft_zdf *z, const uint8_t *buf, size_t len)
{
    memset(z, 0, sizeof *z);
    if (!ft_zdf_is(buf, len)) {
        ft_set_error("not a ZDF");
        return false;
    }
    z->dfs0 = buf[3] == '0';
    size_t header = z->dfs0 ? 28 : 36;
    if (len < header) {
        ft_set_error("ZDF header truncated");
        return false;
    }
    z->buf = buf;
    z->len = len;
    z->w = ft_rd32(buf + 4);
    z->h = ft_rd32(buf + 8);
    z->block_cols = ft_rd32(buf + 12);
    z->block_rows = ft_rd32(buf + 16);
    z->frame_count = ft_rd32(buf + 20);
    if (!z->dfs0) {
        z->x = (int32_t)ft_rd32(buf + 28);
        z->y = (int32_t)ft_rd32(buf + 32);
    }
    z->first_frame = header;
    if (z->w == 0 || z->h == 0 || z->w > 8192 || z->h > 8192 || z->frame_count == 0) {
        ft_set_error("ZDF: bad header %ux%u, %u frames", z->w, z->h, z->frame_count);
        return false;
    }
    if (z->block_cols != (z->w + 3) / 4 || z->block_rows != (z->h + 3) / 4) {
        ft_set_error("ZDF: block grid %ux%u doesn't match %ux%u", z->block_cols, z->block_rows, z->w, z->h);
        return false;
    }

    /* Big enough for the padded keyframe strips and for 4x4 blocks overhanging the edges. */
    size_t key = (size_t)padded_stride(z->w) * (((size_t)z->h + 7) & ~(size_t)7);
    size_t blocks = (size_t)z->block_rows * 4 * z->w * 3 + (size_t)z->block_cols * 12;
    z->fb_size = (key > blocks ? key : blocks) + 64;
    z->fb = calloc(1, z->fb_size);
    if (!z->fb) {
        ft_set_error("out of memory for ZDF frame buffer");
        return false;
    }
    z->pos = z->first_frame;
    return true;
}

void ft_zdf_close(ft_zdf *z)
{
    free(z->fb);
    memset(z, 0, sizeof *z);
}

/* Reads a { u32 tag; u32 size; data } chunk at z->pos. */
static bool read_chunk(ft_zdf *z, uint32_t tag, const uint8_t **data, uint32_t *size)
{
    if (z->len - z->pos < 8 || ft_rd32(z->buf + z->pos) != tag) {
        ft_set_error("ZDF frame %u: expected %.4s chunk at offset %zu", z->next_frame, (const char *)&tag, z->pos);
        return false;
    }
    *size = ft_rd32(z->buf + z->pos + 4);
    if (*size > z->len - z->pos - 8) {
        ft_set_error("ZDF frame %u: %.4s chunk runs past the end", z->next_frame, (const char *)&tag);
        return false;
    }
    *data = z->buf + z->pos + 8;
    z->pos += 8 + (size_t)*size;
    return true;
}

/* kt_zdf_rle_decode_strip (0x433050). Returns false if the strip data runs out. */
static bool rle_strip(const ft_zdf *z, uint8_t *dst, const uint8_t *src, size_t len)
{
    size_t stride = padded_stride(z->w), p = 0;
    int state = 0; /* 0 = need control word, 1 = run, 2 = literal */
    uint32_t count = 0;
    uint8_t b = 0, g = 0, r = 0;
    for (uint32_t x = 0; x < z->w; x++) {
        uint8_t *px = dst + (size_t)x * 3;
        for (int row = 0; row < 8; row++, px += stride) {
            while (state == 0 || count == 0) {
                if (len - p < 2)
                    return false;
                uint16_t c = ft_rd16(src + p);
                if (c & 0x8000) {
                    if (len - p < 5)
                        return false;
                    count = c & 0x7FFF;
                    b = src[p + 2], g = src[p + 3], r = src[p + 4];
                    state = 1;
                    p += 5;
                } else {
                    count = c;
                    state = 2;
                    p += 2;
                }
                if (count == 0)
                    state = 0;
            }
            if (state == 2) {
                if (len - p < 3)
                    return false;
                b = src[p], g = src[p + 1], r = src[p + 2];
                p += 3;
            }
            px[0] = b, px[1] = g, px[2] = r;
            count--;
        }
    }
    return true;
}

static bool decode_keyframe(ft_zdf *z, const uint8_t *img, uint32_t size)
{
    size_t stride = padded_stride(z->w), p = 0;
    uint32_t strips = z->h / 8, rest = z->h % 8;
    for (uint32_t s = 0; s < strips; s++) {
        if (size - p < 4)
            goto truncated;
        uint32_t n = ft_rd32(img + p);
        if (n > size - p - 4)
            goto truncated;
        if (!rle_strip(z, z->fb + (size_t)s * stride * 8, img + p + 4, n)) {
            ft_set_error("ZDF keyframe strip %u: RLE data runs out", s);
            return false;
        }
        p += 4 + (size_t)n;
    }
    if (rest) {
        size_t raw = (size_t)rest * z->w * 3;
        if (size - p < 4 || ft_rd32(img + p) > size - p - 4 || ft_rd32(img + p) < raw)
            goto truncated;
        memcpy(z->fb + (size_t)strips * stride * 8, img + p + 4, raw);
        p += 4 + (size_t)ft_rd32(img + p);
    }
    if (p != size) {
        ft_set_error("ZDF keyframe: %zu of %u imag bytes used", p, size);
        return false;
    }
    return true;

truncated:
    ft_set_error("ZDF keyframe strip data runs past the imag chunk");
    return false;
}

static int8_t sext(uint8_t v, unsigned bits)
{
    return (int8_t)(v & (1u << (bits - 1)) ? v | (uint8_t)(0xFFu << bits) : v);
}

/*
 * kt_zdf_apply_delta_mode1 (0x403ec0): 16 bytes + u16 at +16. Pixel i byte v: bits 0-2 ->
 * signed delta for byte 2, bits 3-5 -> byte 1, bits 6-7 plus bit (15 - i) of the u16 as
 * bit 2 -> byte 0.
 */
static void delta_mode1(uint8_t *block, const uint8_t *src)
{
    uint16_t hi = ft_rd16(src + 16);
    for (int i = 0; i < 16; i++) {
        uint8_t v = src[i];
        uint8_t top = (uint8_t)(((v >> 6) & 3) | ((hi >> (15 - i)) & 1 ? 4 : 0));
        uint8_t *px = block + i * 3;
        px[0] = (uint8_t)(px[0] + sext(top, 3));
        px[1] = (uint8_t)(px[1] + sext((v >> 3) & 7, 3));
        px[2] = (uint8_t)(px[2] + sext(v & 7, 3));
    }
}

/* kt_zdf_apply_delta_mode2 (0x404190): u16 per pixel, bits 10-14 -> byte 0, 5-9 -> byte 1, 0-4 -> byte 2. */
static void delta_mode2(uint8_t *block, const uint8_t *src)
{
    for (int i = 0; i < 16; i++) {
        uint16_t v = ft_rd16(src + i * 2);
        uint8_t *px = block + i * 3;
        px[0] = (uint8_t)(px[0] + sext((v >> 10) & 0x1F, 5));
        px[1] = (uint8_t)(px[1] + sext((v >> 5) & 0x1F, 5));
        px[2] = (uint8_t)(px[2] + sext(v & 0x1F, 5));
    }
}

static bool decode_delta(ft_zdf *z, const uint8_t *flag, uint32_t flag_size, const uint8_t *img, uint32_t size)
{
    static const uint32_t cost[4] = { 48, 18, 32, 0 };
    uint32_t blocks = z->block_cols * z->block_rows;
    if (flag_size < (blocks + 3) / 4) {
        ft_set_error("ZDF frame %u: flag map too small (%u < %u)", z->next_frame, flag_size, (blocks + 3) / 4);
        return false;
    }
    size_t row = (size_t)z->w * 3, p = 0;
    uint8_t block[48];
    for (uint32_t i = 0; i < blocks; i++) {
        unsigned mode = (flag[i / 4] >> ((i % 4) * 2)) & 3;
        z->mode_count[mode]++;
        if (mode == 3)
            continue;
        if (size - p < cost[mode]) {
            ft_set_error("ZDF frame %u: delta payload runs past the imag chunk", z->next_frame);
            return false;
        }
        uint32_t bx = i % z->block_cols, by = i / z->block_cols;
        uint8_t *base = z->fb + (size_t)by * 4 * row + (size_t)bx * 12;
        if (mode == 0) {
            memcpy(block, img + p, 48);
        } else {
            for (int r = 0; r < 4; r++)
                memcpy(block + r * 12, base + r * row, 12);
            if (mode == 1)
                delta_mode1(block, img + p);
            else
                delta_mode2(block, img + p);
        }
        for (int r = 0; r < 4; r++)
            memcpy(base + r * row, block + r * 12, 12);
        p += cost[mode];
    }
    z->delta_slack += (uint32_t)(size - p);
    return true;
}

bool ft_zdf_next(ft_zdf *z, ft_image *out)
{
    if (z->next_frame >= z->frame_count) {
        z->next_frame = 0;
        z->pos = z->first_frame;
    }
    const uint8_t *flag, *img;
    uint32_t flag_size, img_size;
    if (!read_chunk(z, TAG_FLAG, &flag, &flag_size) || !read_chunk(z, TAG_IMAG, &img, &img_size))
        return false;
    bool ok = z->next_frame == 0 ? decode_keyframe(z, img, img_size)
                                 : decode_delta(z, flag, flag_size, img, img_size);
    if (!ok)
        return false;
    z->next_frame++;
    return out ? ft_zdf_to_image(z, out) : true;
}

bool ft_zdf_to_image(const ft_zdf *z, ft_image *out)
{
    if (!ft_image_alloc(out, z->w, z->h))
        return false;
    size_t row = (size_t)z->w * 3;
    for (uint32_t y = 0; y < z->h; y++) {
        const uint8_t *src = z->fb + (size_t)y * row;
        uint8_t *dst = out->bgra + (size_t)(z->h - 1 - y) * z->w * 4;
        for (uint32_t x = 0; x < z->w; x++) {
            dst[x * 4 + 0] = src[x * 3 + 0];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 2];
            dst[x * 4 + 3] = 0xFF;
        }
    }
    return true;
}
