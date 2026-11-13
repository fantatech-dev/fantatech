/*
 * Cinepak ('cvid') video decoder, for the intro movie. Each frame is a set of horizontal strips;
 * each strip carries 256-entry codebooks of 2x2 blocks (V1: one entry scaled up to a 4x4 block,
 * V4: four entries, one per quarter) and then a list of 4x4 blocks that are skipped, V1 or V4.
 * Codebooks and the picture carry over between frames, so frames must be decoded in order.
 *
 * Colour entries are 4 luma values plus a signed u/v pair, turned into RGB as r = y + 2v,
 * g = y - u/2 - v, b = y + 2u (clamped), the same arithmetic as Radius' codec and ffmpeg, whose
 * output `ftconv movie --raw` matches byte for byte.
 */
#pragma once

#include "core/ft_common.h"

#define FT_CINEPAK_STRIPS 32

typedef uint8_t ft_cinepak_entry[12]; /* 2x2 block, RGB24, row-major */

typedef struct ft_cinepak {
    uint32_t w, h;
    uint8_t *rgb;               /* the current picture, RGB24, stride w * 3 */
    ft_cinepak_entry v1[FT_CINEPAK_STRIPS][256];
    ft_cinepak_entry v4[FT_CINEPAK_STRIPS][256];
} ft_cinepak;

/* The context is large (~200 KB of codebooks): allocate it, don't put it on the stack. */
bool ft_cinepak_init(ft_cinepak *c, uint32_t w, uint32_t h);
void ft_cinepak_free(ft_cinepak *c);

/* Decodes one frame into c->rgb. An empty frame (no strips) leaves the picture as it was. */
bool ft_cinepak_decode(ft_cinepak *c, const uint8_t *data, size_t size);
