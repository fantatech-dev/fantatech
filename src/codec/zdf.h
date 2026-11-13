/*
 * ZDF ("DFS0"/"DFS1") animated CGs (event*, film*).
 *
 *   header: "DFS1" (36 bytes) or "DFS0" (28 bytes; kt_zdf_seek_to_chunks 0x4015e0)
 *     +4 width  +8 height  +12 block_cols  +16 block_rows  +20 frame_count
 *     +24 total flag-chunk bytes  +28 x  +32 y (DFS1 only)
 *   x/y place the clip within the CG area: full-frame clips are 0,0, overlays (e.g.
 *   film1024 C007A, 584x160 at 344,0) sit over the background.
 *   frames: { "flag" u32 size, data[size]; "imag" u32 size, data[size] } x frame_count
 *
 * Frame 0 is a keyframe (kt_zdf_decode_keyframe 0x401620): its imag chunk is height/8
 * RLE strips, each { u32 len; data[len] }, decoded column-major (8 pixels down per column,
 * run/literal state kept across columns, reset per strip) into rows of the 4-byte-padded
 * stride; then, if height % 8, one more { u32 len; raw } strip copied as (h%8)*w*3
 * contiguous bytes. RLE control: u16 c; c & 0x8000 ? run of (c & 0x7fff) x {B,G,R}
 * : c literal {B,G,R} pixels.
 *
 * Later frames are deltas (kt_zdf_decode_deltaframe 0x4016a0) on 4x4 blocks with an
 * unpadded w*3 stride: a 2-bit mode per block (row-major, low bits first) from the flag
 * chunk; payload in the imag chunk:
 *   0: 48 bytes, raw block      1: 18 bytes, small delta      2: 32 bytes, larger delta
 *   3: no data, block unchanged
 * See zdf.c for the delta bit layouts. Buffer row 0 is the BOTTOM of the image.
 */
#pragma once

#include "codec/image.h"

typedef struct ft_zdf {
    const uint8_t *buf;     /* borrowed; must outlive the decoder */
    size_t len;
    bool dfs0;
    uint32_t w, h;
    uint32_t block_cols, block_rows;
    uint32_t frame_count;
    int32_t x, y;           /* placement within the CG area (DFS1; 0 for DFS0) */
    size_t first_frame;     /* offset of frame 0's flag chunk */

    /* playback state */
    uint32_t next_frame;
    size_t pos;
    uint8_t *fb;            /* frame buffer, BGR, layout as described above */
    size_t fb_size;

    /* stats for ftconv verify */
    uint32_t delta_slack;   /* bytes left over in delta imag chunks */
    uint32_t mode_count[4];
} ft_zdf;

bool ft_zdf_is(const uint8_t *buf, size_t len);
bool ft_zdf_open(ft_zdf *z, const uint8_t *buf, size_t len);
void ft_zdf_close(ft_zdf *z);

/* Decodes the next frame (keyframe again after the last one). `out` may be NULL. */
bool ft_zdf_next(ft_zdf *z, ft_image *out);

/* Copies the current frame buffer into a BGRA top-down image. */
bool ft_zdf_to_image(const ft_zdf *z, ft_image *out);
