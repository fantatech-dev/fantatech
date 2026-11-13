/*
 * Game asset resolver: opens every archive once and finds assets by role (face, CG,
 * text line, sound, music) the way the scripts name them. No SDL.
 *
 * Quality tiers follow ktxp01.ini defaults: EVENT_TEXTURE_SIZE=2 -> eve1024 (1024x554),
 * FACE_TEXTURE_SIZE=2 -> face512 (228x256), with fallbacks to the other tiers.
 */
#pragma once

#include "codec/image.h"
#include "codec/zdf.h"
#include "script/script.h"
#include "vfs/archive.h"

typedef enum ft_arc {
    FT_ARC_BG02, FT_ARC_EVE1024, FT_ARC_EVE2048, FT_ARC_EVENT, FT_ARC_FACE, FT_ARC_FACE1024, FT_ARC_FACE512,
    FT_ARC_FILM1024, FT_ARC_FILM2048, FT_ARC_FRAME, FT_ARC_MUSIC, FT_ARC_SE, FT_ARC_STAFROLL, FT_ARC_TEXT,
    FT_ARC_TEXT3D, FT_ARC_TEXTBMP, FT_ARC_VOICE, FT_ARC_COUNT
} ft_arc;

typedef struct ft_assets {
    char dir[512];
    ft_archive arc[FT_ARC_COUNT];
    ft_script *scripts;     /* every script in text + text3d, parsed */
    uint32_t script_count;
} ft_assets;

/* A loaded raw entry (caller frees data). */
typedef struct ft_blob {
    uint8_t *data;
    uint32_t size;
    ft_arc arc;             /* where it was found */
} ft_blob;

/*
 * Screen pixels per picture pixel for pictures from `arc` (FUN_00432d10): event-size art
 * (740 wide) x 1024/740, the 2048 sets x 1024/1894, everything else x 1.
 */
float ft_assets_scale(ft_arc arc);

bool ft_assets_open(ft_assets *a, const char *dir);
void ft_assets_close(ft_assets *a);

const ft_script *ft_assets_script(const ft_assets *a, const char *name);

/* Reads `name` (exact entry name, case-insensitive) from one archive. */
bool ft_assets_read(ft_assets *a, ft_arc arc, const char *name, ft_blob *out);

/*
 * Image lookups by script code (no extension). Each fills `out` with the raw entry, which
 * is either a pseudo-BMP or a ZDF; use ft_assets_decode to get the first frame.
 */
bool ft_assets_face(ft_assets *a, const char *code, ft_blob *out);
bool ft_assets_cg(ft_assets *a, const char *code, ft_blob *out);      /* %s / %event / bg.open */
bool ft_assets_bg02(ft_assets *a, const char *code, ft_blob *out);    /* bg02.open, falls back to CG */
bool ft_assets_film(ft_assets *a, const char *code, ft_blob *out);    /* %film.load */
bool ft_assets_textline(ft_assets *a, const char *id, ft_blob *out);  /* dialogue $id -> textbmp */

/* Decodes a pseudo-BMP or the first frame of a ZDF. */
bool ft_assets_decode(const ft_blob *b, ft_image *out);

/* %sound.play code: voice.bin first, then se.bin. Music by track stem (music.bin). */
bool ft_assets_sound(ft_assets *a, const char *code, ft_blob *out);
bool ft_assets_music(ft_assets *a, const char *stem, ft_blob *out);
