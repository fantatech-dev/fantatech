/*
 * Drawing kit for our own screens (menus, save panel, settings): plain boxes and text on a dark
 * veil, light grey with a muted blue for the selection, red for warnings. All coordinates are in
 * the 1024x768 logical screen.
 */
#pragma once

#include <SDL3/SDL.h>

#include "game/i18n.h"

#define FT_UI_W 1024.0f
#define FT_UI_H 768.0f

#define FT_UI_ACCENT ((SDL_Color){ 86, 132, 204, 255 })
#define FT_UI_DIM ((SDL_Color){ 150, 150, 158, 255 })
#define FT_UI_FAINT ((SDL_Color){ 70, 70, 78, 255 })
#define FT_UI_TEXT ((SDL_Color){ 236, 236, 236, 255 })
#define FT_UI_RED ((SDL_Color){ 200, 56, 56, 255 })
#define FT_UI_BLACK ((SDL_Color){ 0, 0, 0, 255 })

#define FT_UI_CACHE 48

typedef struct ft_tex {
    SDL_Texture *t;
    float w, h;
} ft_tex;

typedef struct ft_ui {
    SDL_Renderer *r;
    const ft_i18n *i18n;        /* translations for ft_ui_text, or NULL */
    struct {
        const char *s;
        int px;
        ft_tex t;
    } cache[FT_UI_CACHE];       /* UI strings by (pointer, size) */
    int cached;
} ft_ui;

/* Drops the cached UI strings (after the language changes). */
void ft_ui_clear(ft_ui *u);

ft_tex ft_ui_render_text(ft_ui *u, const char *utf8, int px);
void ft_tex_free(ft_tex *t);
/* `jp` translated by the current table, or `jp` itself. */
const char *ft_ui_tr(const ft_ui *u, const char *jp);
/* A UI string (one of ft_ui_strings, so the pointer identifies it), translated and cached. */
const ft_tex *ft_ui_text(ft_ui *u, const char *jp, int px);

/* Draws `t` tinted `c`, left-aligned at x (or centred on x), vertically centred on cy; max_w > 0 clips. */
void ft_ui_draw_tex(ft_ui *u, const ft_tex *t, float x, float cy, SDL_Color c, bool centre, float max_w);
void ft_ui_draw_text(ft_ui *u, const char *jp, int px, float x, float cy, SDL_Color c, bool centre);
/* Plain (untranslated) text rendered for this one draw. */
void ft_ui_draw_label(ft_ui *u, const char *utf8, int px, float x, float cy, SDL_Color c, bool centre);

SDL_Color ft_ui_alpha(SDL_Color c, float a);

void ft_ui_fill(ft_ui *u, SDL_FRect r, SDL_Color c);
void ft_ui_outline(ft_ui *u, SDL_FRect r, SDL_Color c);
/* A circle of radius `rad` around (cx, cy), filled or as an outline (radio buttons). */
void ft_ui_circle(ft_ui *u, float cx, float cy, float rad, bool filled, SDL_Color c);
/* A box: dark when idle, lighter with a white edge when `hot`, filled with `c` when `on`. */
void ft_ui_box(ft_ui *u, SDL_FRect r, bool on, bool hot, SDL_Color c);
/* A button: ft_ui_box with the UI string `jp` centred on it. */
void ft_ui_button(ft_ui *u, SDL_FRect r, const char *jp, int px, bool on, bool hot, SDL_Color c);
/* A menu entry: a box with `label` centred, the selection lighter, a disabled entry dimmed. */
void ft_ui_item(ft_ui *u, SDL_FRect r, const ft_tex *label, bool sel, bool enabled);

/* The screen backdrop: a dark veil over the scene. */
void ft_ui_backdrop(ft_ui *u);

bool ft_ui_inside(SDL_FRect r, float x, float y);
