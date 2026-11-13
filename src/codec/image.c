#include "codec/image.h"

#include <stdlib.h>

#define FT_IMAGE_MAX_DIM 8192

bool ft_image_alloc(ft_image *img, uint32_t w, uint32_t h)
{
    img->w = img->h = 0;
    img->bgra = NULL;
    if (w == 0 || h == 0 || w > FT_IMAGE_MAX_DIM || h > FT_IMAGE_MAX_DIM) {
        ft_set_error("bad image size %ux%u", w, h);
        return false;
    }
    img->bgra = calloc((size_t)w * h, 4);
    if (!img->bgra) {
        ft_set_error("out of memory for %ux%u image", w, h);
        return false;
    }
    img->w = w;
    img->h = h;
    return true;
}

void ft_image_free(ft_image *img)
{
    free(img->bgra);
    img->bgra = NULL;
    img->w = img->h = 0;
}
