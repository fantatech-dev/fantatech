/*
 * Decoded image: 8-bit BGRA, top-down rows, stride = w * 4 (SDL_PIXELFORMAT_BGRA32).
 */
#pragma once

#include "core/ft_common.h"

typedef struct ft_image {
    uint32_t w, h;
    uint8_t *bgra;
} ft_image;

bool ft_image_alloc(ft_image *img, uint32_t w, uint32_t h);
void ft_image_free(ft_image *img);
