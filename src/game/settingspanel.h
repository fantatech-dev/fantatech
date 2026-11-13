/*
 * The settings screen (our addition): language, four volumes (master, music, voice, sound
 * effects) and fullscreen. It replaces the original's system01 menu
 * (screen resolution and texture sizes, which mean nothing here) behind the title menu's
 * システム設定, and opens from the save panel's 設定 button during play. Changes apply at
 * once; the runtime stores them.
 */
#pragma once

#include <SDL3/SDL.h>

#include "game/config.h"
#include "game/i18n.h"

#define FT_SETTINGS_LANGS 8

typedef struct ft_lang_option {
    char id[32];                /* i18n table name, "" for the original Japanese */
    char name[64];              /* as listed */
} ft_lang_option;

typedef enum ft_settings_action {
    FT_SETTINGS_NONE,
    FT_SETTINGS_LANGUAGE,       /* config().lang changed */
    FT_SETTINGS_VOLUME,         /* a volume changed */
    FT_SETTINGS_DISPLAY,        /* fullscreen changed */
    FT_SETTINGS_CLOSE,
} ft_settings_action;

typedef struct ft_settingspanel ft_settingspanel;

ft_settingspanel *ft_settingspanel_create(SDL_Renderer *r);
void ft_settingspanel_destroy(ft_settingspanel *p);

void ft_settingspanel_open(ft_settingspanel *p, const ft_config *cfg, const ft_lang_option *langs, int count,
                           const ft_i18n *i18n);
void ft_settingspanel_close(ft_settingspanel *p);
bool ft_settingspanel_is_open(const ft_settingspanel *p);

ft_settings_action ft_settingspanel_event(ft_settingspanel *p, const SDL_Event *ev);
/* The settings as the player has set them so far. */
const ft_config *ft_settingspanel_config(const ft_settingspanel *p);
/* F11 toggled fullscreen while the panel is open: show it, and keep it when the panel closes. */
void ft_settingspanel_set_fullscreen(ft_settingspanel *p, bool on);
/* Re-renders the text after the language changed. */
void ft_settingspanel_retranslate(ft_settingspanel *p);

void ft_settingspanel_render(ft_settingspanel *p);
