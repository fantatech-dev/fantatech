/*
 * The in-game menus (our addition): the %cmnd command menu and the story's choices (%select.wait,
 * select), both as a plain centred column of boxes over the CG, like a usual visual novel.
 */
#pragma once

#include <SDL3/SDL.h>

#include "game/i18n.h"
#include "script/interp.h"

typedef enum ft_choice_kind {
    FT_CHOICE_COMMAND,          /* %cmnd entries, "verb[.call][.N]: label" (see interp.c) */
    FT_CHOICE_SELECT,           /* plain texts */
} ft_choice_kind;

typedef struct ft_choicemenu ft_choicemenu;

ft_choicemenu *ft_choicemenu_create(SDL_Renderer *r);
void ft_choicemenu_destroy(ft_choicemenu *m);

void ft_choicemenu_open(ft_choicemenu *m, ft_choice_kind kind, const ft_choice *c, int count, const ft_i18n *i18n);
void ft_choicemenu_close(ft_choicemenu *m);
bool ft_choicemenu_is_open(const ft_choicemenu *m);

/* Feeds an input event; returns the picked entry, or -1. */
int ft_choicemenu_event(ft_choicemenu *m, const SDL_Event *ev);

void ft_choicemenu_render(ft_choicemenu *m);
