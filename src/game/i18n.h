/*
 * Translation tables (i18n/<lang>.txt). No SDL.
 *
 * One entry per line, UTF-8:
 *
 *     # comment
 *     @font: C:\Windows\Fonts\arial.ttf     optional: font for translated text
 *     a21003: The die is cast.               dialogue: the script's $id
 *     9f3c12ab: Start from the beginning     anything else: FNV-1a hash of the Japanese
 *
 * Keys are a dialogue $id or 8 hex digits of ft_i18n_hash() over the original UTF-8 string
 * (as the script has it, quotes stripped). An empty text means "not translated yet". In the
 * text, "\n" forces a line break. `ftconv i18n-template` writes every key with the Japanese
 * as a comment.
 */
#pragma once

#include "core/ft_common.h"

typedef struct ft_i18n_entry {
    char *key;
    char *text;
} ft_i18n_entry;

typedef struct ft_i18n {
    ft_i18n_entry *e;           /* sorted by key */
    uint32_t count;
    char font[512];             /* @font, "" if none */
    char name[64];              /* @name: the language as the settings screen lists it, "" if none */
} ft_i18n;

/* 32-bit FNV-1a over the bytes of `utf8`. */
uint32_t ft_i18n_hash(const char *utf8);

/* The table key for a string without a $id: 8 lowercase hex digits. */
void ft_i18n_key(const char *original, char out[9]);

bool ft_i18n_load(ft_i18n *t, const char *path);
void ft_i18n_free(ft_i18n *t);

/* Translation by key, or NULL. */
const char *ft_i18n_get(const ft_i18n *t, const char *key);
/* Translation of `original` by its hash, or NULL. */
const char *ft_i18n_text(const ft_i18n *t, const char *original);

/* The engine's own UI text (save panel), keyed like menus; ftconv i18n-template lists it. */
extern const char *const ft_ui_strings[];
extern const size_t ft_ui_string_count;
