/*
 * A plain column of menu boxes (our addition), centred on the screen at a given height: the title
 * screen's menus (starmenu.txt's select02 / selectCancel) and the Esc menu. The runtime picks which
 * entries are available (Continue needs a save, Album isn't done); unavailable ones are greyed out.
 */
#pragma once

#include <SDL3/SDL.h>

#include "game/i18n.h"

#define FT_TITLEMENU_ITEMS 8

typedef struct ft_titlemenu ft_titlemenu;

ft_titlemenu *ft_titlemenu_create(SDL_Renderer *r);
void ft_titlemenu_destroy(ft_titlemenu *m);

/* `texts` are (Japanese) entries; `enabled` may be NULL (all available); the column is centred on y `cy`. */
void ft_titlemenu_open(ft_titlemenu *m, const char *const *texts, const bool *enabled, int count, float cy,
                       const ft_i18n *i18n);
void ft_titlemenu_close(ft_titlemenu *m);
bool ft_titlemenu_is_open(const ft_titlemenu *m);

/* Feeds an input event; returns the picked entry, or -1. */
int ft_titlemenu_event(ft_titlemenu *m, const SDL_Event *ev);
/* The highlighted entry (autoplay takes it). */
int ft_titlemenu_selected(const ft_titlemenu *m);
/* Highlights entry `i` if it's available. */
void ft_titlemenu_select(ft_titlemenu *m, int i);

void ft_titlemenu_render(ft_titlemenu *m);
