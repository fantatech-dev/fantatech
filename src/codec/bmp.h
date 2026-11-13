/*
 * Kotetsu pseudo-BMP (".BMP" entries in face*, bg02, frame, event*, stafroll, textbmp).
 *
 *   +0   u32 magic     0x88888888 = opaque, 0x88888899 = with alpha
 *   +4   u32 width
 *   +8   u32 height
 *   +12  u32 BMP file size (copy of the embedded header's field)
 *   +16  54-byte BITMAPFILEHEADER + BITMAPINFOHEADER, copied verbatim into the output
 *   +70  tag[tiles]    one byte per 8x8 tile, raster order, tiles = ceil(w/8) * ceil(h/8)
 *        dc[tiles]     3 bytes (B,G,R) per tile; 4 (B,G,R,default alpha) with alpha
 *        payload       per-tile pixel data, variable size
 *
 * Tag low nibble = colour mode (0-7), see bmp.c. With alpha, high nibble 0x10 = every
 * pixel uses the DC alpha byte, anything else = one alpha byte per pixel stored right
 * after the tile's colour bytes (only 0x20 is counted by the original's cost function).
 *
 * Tiles are placed in BMP buffer order, i.e. tile row 0 is the BOTTOM of the image.
 * Traced from mana01: kt_bmp_check_magic_raw/packed (0x438100 / 0x4394b0), the tile drivers
 * (0x4384c0 / 0x439b30), the pixel decoders (0x4381a0 / 0x439550) and the tile blitters
 * (0x438410 / 0x439a70).
 */
#pragma once

#include "codec/image.h"

#define FT_BMP_MAGIC_OPAQUE 0x88888888u
#define FT_BMP_MAGIC_ALPHA 0x88888899u

typedef struct ft_bmp_info {
    bool has_alpha;
    uint32_t bytes_used;     /* must equal the entry size */
    uint32_t file_size_field;
    int32_t bmp_width, bmp_height;
    uint16_t bmp_bpp;
    uint32_t odd_alpha_tiles; /* alpha tiles whose high nibble is neither 0x10 nor 0x20 */
    uint32_t mode_count[8];
} ft_bmp_info;

bool ft_bmp_is(const uint8_t *buf, size_t len);

/* Decodes into a BGRA top-down image. `info` may be NULL. */
bool ft_bmp_decode(const uint8_t *buf, size_t len, ft_image *out, ft_bmp_info *info);
