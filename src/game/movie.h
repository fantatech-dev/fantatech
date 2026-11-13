/*
 * The intro movie player (start.txt: movieplay.load movie0508_cinepak.avi, movieplay.wait): the
 * AVI's Cinepak frames on a streaming texture, timed by the caller's clock, and its MS ADPCM audio,
 * decoded whole at open and queued on its own stream. Pausing the clock and the audio device keeps
 * the two together.
 */
#pragma once

#include <SDL3/SDL.h>

typedef struct ft_movie ft_movie;

/* Opens `path` and starts it at `now_ms`; audio goes to `device` (0: silent) at `gain`. */
ft_movie *ft_movie_open(SDL_Renderer *r, SDL_AudioDeviceID device, const char *path, float gain, uint64_t now_ms);
void ft_movie_close(ft_movie *m);

void ft_movie_set_gain(ft_movie *m, float gain);

/* Decodes up to the frame due at `now_ms`; returns false once the last frame's time is over. */
bool ft_movie_update(ft_movie *m, uint64_t now_ms);

/* Draws the current frame into `dst`. */
void ft_movie_render(ft_movie *m, const SDL_FRect *dst);

/* The frame size, for placing it. */
void ft_movie_size(const ft_movie *m, float *w, float *h);
