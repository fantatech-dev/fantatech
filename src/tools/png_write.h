/*
 * Minimal PNG writer for ftconv: 8-bit RGBA, Paeth row filters, fixed-Huffman deflate
 * with an LZ77 hash chain. No dependencies.
 */
#pragma once

#include "codec/image.h"

bool ft_png_write(const char *path, const ft_image *img);
