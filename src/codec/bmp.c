#include "codec/bmp.h"

#include <string.h>

#define HEADER_SIZE 70

bool ft_bmp_is(const uint8_t *buf, size_t len)
{
    if (len < 4)
        return false;
    uint32_t magic = ft_rd32(buf);
    return magic == FT_BMP_MAGIC_OPAQUE || magic == FT_BMP_MAGIC_ALPHA;
}

/* Colour payload bytes for `n` pixels in `mode` (both paths). */
static uint32_t color_cost(unsigned mode, uint32_t n)
{
    switch (mode) {
    case 0: return 0;
    case 1: case 2: case 3: case 5: return n;
    case 4: return (n + 1) / 2;
    case 6: return n * 2;
    case 7: return n * 3;
    default: return 0;
    }
}

/*
 * Decodes one tile's colours into `tile` (BGRA, n pixels). Arithmetic wraps at 8 bits like
 * the original's byte adds. Modes 5 and 7 ignore the DC colour; mode 4 adds only DC blue.
 */
static void decode_colors(unsigned mode, const uint8_t *src, const uint8_t *dc, uint32_t n, uint8_t *tile)
{
    uint8_t db = dc[0], dg = dc[1], dr = dc[2];
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *px = tile + i * 4;
        uint8_t v;
        switch (mode) {
        case 0:
            px[0] = db, px[1] = dg, px[2] = dr;
            break;
        case 1: /* B3 G3 R2 */
            v = src[i];
            px[0] = (uint8_t)((v & 7) + db), px[1] = (uint8_t)(((v >> 3) & 7) + dg), px[2] = (uint8_t)((v >> 6) + dr);
            break;
        case 2: /* B2 G3 R3 */
            v = src[i];
            px[0] = (uint8_t)((v & 3) + db), px[1] = (uint8_t)(((v >> 2) & 7) + dg), px[2] = (uint8_t)((v >> 5) + dr);
            break;
        case 3: /* B3 G2 R3 */
            v = src[i];
            px[0] = (uint8_t)((v & 7) + db), px[1] = (uint8_t)(((v >> 3) & 3) + dg), px[2] = (uint8_t)((v >> 5) + dr);
            break;
        case 4: /* 4-bit grey, low nibble first, added to DC blue */
            v = (uint8_t)(((i & 1) ? src[i / 2] >> 4 : src[i / 2] & 0xF) + db);
            px[0] = px[1] = px[2] = v;
            break;
        case 5: /* 8-bit grey, no DC */
            px[0] = px[1] = px[2] = src[i];
            break;
        case 6: { /* B5 G5 R5, little-endian u16 */
            uint16_t w = ft_rd16(src + i * 2);
            px[0] = (uint8_t)((w & 0x1F) + db), px[1] = (uint8_t)(((w >> 5) & 0x1F) + dg),
            px[2] = (uint8_t)(((w >> 10) & 0x1F) + dr);
            break;
        }
        case 7: /* raw BGR, no DC */
            px[0] = src[i * 3], px[1] = src[i * 3 + 1], px[2] = src[i * 3 + 2];
            break;
        }
        px[3] = 0xFF;
    }
}

bool ft_bmp_decode(const uint8_t *buf, size_t len, ft_image *out, ft_bmp_info *info)
{
    ft_bmp_info local;
    if (!info)
        info = &local;
    memset(info, 0, sizeof *info);
    out->bgra = NULL;

    if (len < HEADER_SIZE || !ft_bmp_is(buf, len)) {
        ft_set_error("not a pseudo-BMP");
        return false;
    }
    info->has_alpha = ft_rd32(buf) == FT_BMP_MAGIC_ALPHA;
    uint32_t w = ft_rd32(buf + 4), h = ft_rd32(buf + 8);
    info->file_size_field = ft_rd32(buf + 12);
    info->bmp_width = (int32_t)ft_rd32(buf + 16 + 18);
    info->bmp_height = (int32_t)ft_rd32(buf + 16 + 22);
    info->bmp_bpp = ft_rd16(buf + 16 + 28);

    if (!ft_image_alloc(out, w, h))
        return false;

    uint32_t tiles_x = (w + 7) / 8, tiles_y = (h + 7) / 8;
    uint64_t tiles = (uint64_t)tiles_x * tiles_y;
    uint32_t dc_size = info->has_alpha ? 4 : 3;
    uint64_t payload_start = HEADER_SIZE + tiles * (1 + dc_size);
    if (payload_start > len) {
        ft_set_error("pseudo-BMP %ux%u: tile tables run past the end", w, h);
        goto fail;
    }
    const uint8_t *tags = buf + HEADER_SIZE;
    const uint8_t *dcs = tags + tiles;
    size_t pos = (size_t)payload_start;

    uint8_t tile[8 * 8 * 4];
    uint32_t t = 0;
    for (uint32_t ty = 0; ty < tiles_y; ty++) {
        uint32_t y0 = ty * 8, th = h - y0 < 8 ? h - y0 : 8;
        for (uint32_t tx = 0; tx < tiles_x; tx++, t++) {
            uint32_t x0 = tx * 8, tw = w - x0 < 8 ? w - x0 : 8;
            uint32_t n = tw * th;
            uint8_t tag = tags[t];
            const uint8_t *dc = dcs + (size_t)t * dc_size;
            unsigned mode = info->has_alpha ? (tag & 0xFu) : tag;
            if (mode > 7) {
                ft_set_error("pseudo-BMP tile %u: bad mode tag 0x%02x", t, tag);
                goto fail;
            }
            info->mode_count[mode]++;

            uint32_t ccost = color_cost(mode, n);
            bool per_pixel_alpha = info->has_alpha && (tag & 0xF0) != 0x10;
            if (per_pixel_alpha && (tag & 0xF0) != 0x20)
                info->odd_alpha_tiles++;
            uint32_t cost = ccost + (per_pixel_alpha ? n : 0);
            if (cost > len - pos) {
                ft_set_error("pseudo-BMP tile %u payload runs past the end", t);
                goto fail;
            }
            decode_colors(mode, buf + pos, dc, n, tile);
            if (info->has_alpha) {
                for (uint32_t i = 0; i < n; i++)
                    tile[i * 4 + 3] = per_pixel_alpha ? buf[pos + ccost + i] : dc[3];
            }
            pos += cost;

            /* Tile pixel i sits at buffer row y0 + i / tw; buffer row 0 is the bottom. */
            for (uint32_t i = 0; i < n; i++) {
                uint32_t x = x0 + i % tw, y = y0 + i / tw;
                memcpy(out->bgra + ((size_t)(h - 1 - y) * w + x) * 4, tile + i * 4, 4);
            }
        }
    }
    info->bytes_used = (uint32_t)pos;
    return true;

fail:
    ft_image_free(out);
    return false;
}
