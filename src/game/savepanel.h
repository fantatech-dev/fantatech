/*
 * The save / load panel: our own design, not the original's savepanel (which this replaces
 * behind the same script commands, see runtime.c).
 *
 * A page shows 8 slots as cards (screenshot, save point, date) in 2 columns; 6 pages hold
 * FT_SAVE_SLOTS. Opened from a scene's system verb it saves and loads (Tab switches); opened
 * from the title menu it only loads. Overwriting, deleting and loading over a game in
 * progress ask first. The panel only picks: the runtime does the saving and loading.
 */
#pragma once

#include <SDL3/SDL.h>

#include "game/i18n.h"

typedef struct ft_savepanel ft_savepanel;

typedef enum ft_panel_action {
    FT_PANEL_NONE,
    FT_PANEL_SAVE,      /* save into *slot (confirmed if it held a save) */
    FT_PANEL_LOAD,      /* load *slot */
    FT_PANEL_DELETE,    /* delete *slot */
    FT_PANEL_BACK,      /* leave the panel */
    FT_PANEL_SETTINGS,  /* open the settings screen over the panel */
} ft_panel_action;

/* `dir` holds the NN.sav / NN.png slot files. */
ft_savepanel *ft_savepanel_create(SDL_Renderer *r, const char *dir);
void ft_savepanel_destroy(ft_savepanel *p);

/* Reads the slots and shows the panel; `can_save` false is the title menu's load-only panel. */
void ft_savepanel_open(ft_savepanel *p, bool can_save, const ft_i18n *i18n);
void ft_savepanel_close(ft_savepanel *p);
bool ft_savepanel_is_open(const ft_savepanel *p);

/* Feeds an input event; returns what the player chose, with the 1-based slot in *slot. */
ft_panel_action ft_savepanel_event(ft_savepanel *p, const SDL_Event *ev, int *slot);

/* Re-reads one slot's files after the runtime saved or deleted it. */
void ft_savepanel_refresh(ft_savepanel *p, int slot);

/* Re-renders the panel's text after the language changed. */
void ft_savepanel_retranslate(ft_savepanel *p);

/* Shows a short message (Japanese UI text, translated like the rest) in the header. */
void ft_savepanel_notice(ft_savepanel *p, const char *text);

void ft_savepanel_render(ft_savepanel *p);
