/*
 * Player settings (our addition): language, volumes and fullscreen, kept in a small text file
 * ("key value" per line) in the per-user data folder.
 */
#pragma once

#include "core/ft_common.h"

enum { FT_VOL_MASTER, FT_VOL_MUSIC, FT_VOL_VOICE, FT_VOL_SE, FT_VOL_COUNT };

typedef struct ft_config {
    char lang[32];              /* i18n table name, "" for the original Japanese */
    int volume[FT_VOL_COUNT];   /* 0..100 */
    bool fullscreen;            /* borderless fullscreen (default) or a window; F11 toggles */
} ft_config;

void ft_config_default(ft_config *c);
/* Reads `path` over the defaults; false (defaults kept) if there's no such file. */
bool ft_config_load(ft_config *c, const char *path);
bool ft_config_save(const ft_config *c, const char *path);
/* The gain for a sound in volume group `group`: master x group, squared for an even feel. */
float ft_config_gain(const ft_config *c, int group);
