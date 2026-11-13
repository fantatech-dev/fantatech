/*
 * Game runtime: the SDL host for the script interpreter. Owns the scene layers, dialogue
 * window, menus, timers and audio, and maps script commands onto them.
 */
#pragma once

#include <SDL3/SDL.h>

typedef struct ft_runtime ft_runtime;

/* Opens the game data in `game_dir` and boots the scripts (start.txt unless `script`). */
ft_runtime *ft_runtime_create(SDL_Renderer *renderer, const char *game_dir, const char *script);
void ft_runtime_destroy(ft_runtime *rt);

void ft_runtime_event(ft_runtime *rt, const SDL_Event *ev);
void ft_runtime_update(ft_runtime *rt);
/* Draws the frame; the caller presents it (so it can read pixels first). */
void ft_runtime_render(ft_runtime *rt);

/* Autoplay: advance dialogue, skip waits and take the first menu entry without input. */
void ft_runtime_set_autoplay(ft_runtime *rt, bool on);
/*
 * Developer tools, off unless the exe gets --dev (or FT_DEV=1): the dev overlay on the ` key (info
 * page, C bug report, R replay, S scene picker; FT_DEV_UI=1/2 opens it at start) and Ctrl
 * fast-forward. Players don't get them.
 */
void ft_runtime_set_dev(ft_runtime *rt, bool on);
/* Dialogue lines shown so far. */
uint32_t ft_runtime_dialogue_count(const ft_runtime *rt);

/*
 * Loads i18n/<lang>.txt (working directory, next to the exe, or the repo root) and shows its
 * translations from then on; NULL / "" goes back to the original Japanese.
 */
bool ft_runtime_set_language(ft_runtime *rt, const char *lang);

/* Puts the window in or out of fullscreen as the player's settings say (default: fullscreen). */
void ft_runtime_apply_display(ft_runtime *rt);

/* Restarts at a label of the running script with a cleared scene, like the scene picker. */
bool ft_runtime_jump_label(ft_runtime *rt, const char *label);

/* True once the scripts have ended (or failed). */
bool ft_runtime_finished(const ft_runtime *rt);
