/*
 * Text rasterizer for menus and translated dialogue (original dialogue comes from textbmp
 * images). SDL_ttf with two fonts:
 *   - the Japanese gothic: MS Gothic, the font ktxp01.ini names, from the player's Windows fonts,
 *     or the platform's stock Japanese gothic elsewhere;
 *   - for other languages, a proportional Latin font (Tahoma, Helvetica, DejaVu Sans, ...) in
 *     front of it, with the gothic as SDL_ttf's fallback for any Japanese characters.
 * FT_FONT=path replaces both, and a translation table's @font replaces the Latin one. Output:
 * white glyphs with anti-aliased coverage in alpha, BGRA.
 */
#pragma once

#include "codec/image.h"

/* Opens the Japanese gothic; false (and ft_get_error) if no font file could be loaded. */
bool ft_font_open(void);
/* Switches to the font file at `path`, with the gothic behind it (keeps the current fonts if it
 * can't be opened). */
bool ft_font_open_file(const char *path);
/* Goes back to the default fonts for a language: `latin` false is the Japanese gothic alone, true
 * a Latin font in front of it (or the gothic alone when no Latin font is found). */
bool ft_font_reset(bool latin);
void ft_font_close(void);
/* The fonts in use (family, style and file) and the SDL_ttf version, for bug reports. */
void ft_font_describe(char *out, size_t cap);
bool ft_font_render(const char *utf8, int pixel_height, ft_image *out);
/* Word-wrapped to `wrap_w` pixels, lines centred; "\n" breaks a line. */
bool ft_font_render_wrapped(const char *utf8, int pixel_height, int wrap_w, ft_image *out);
