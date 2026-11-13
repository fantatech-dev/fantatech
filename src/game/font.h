/*
 * Text rasterizer for menus, speaker names and translated dialogue (original dialogue comes
 * from textbmp images). SDL_ttf with MS Gothic, the font ktxp01.ini names, read from the
 * player's own Windows fonts (or FT_FONT=path, or a translation table's @font). Output:
 * white glyphs with anti-aliased coverage in alpha, BGRA.
 */
#pragma once

#include "codec/image.h"

/* Opens the font; false (and ft_get_error) if no font file could be loaded. */
bool ft_font_open(void);
/* Switches to the font file at `path` (keeps the current one if it can't be opened). */
bool ft_font_open_file(const char *path);
/* Goes back to the default font (FT_FONT or the platform's Japanese gothic) after ft_font_open_file. */
bool ft_font_reset(void);
void ft_font_close(void);
bool ft_font_render(const char *utf8, int pixel_height, ft_image *out);
/* Word-wrapped to `wrap_w` pixels, lines centred; "\n" breaks a line. */
bool ft_font_render_wrapped(const char *utf8, int pixel_height, int wrap_w, ft_image *out);
