/*
 * AVI (RIFF 'AVI ') reader, enough for the intro movie (movie0508_cinepak.avi: one Cinepak video
 * stream, one MS ADPCM audio stream, OpenDML-style but under 1 GB, so everything is in the first
 * RIFF). The header lists are parsed at open; the movi list is walked once for the offsets of every
 * video and audio chunk, which are then read on demand.
 */
#pragma once

#include "core/ft_common.h"

#define FT_AVI_MAX_COEFS 32

typedef struct ft_avi_chunk {
    uint64_t offset;            /* of the chunk's data */
    uint32_t size;
} ft_avi_chunk;

/* WAVEFORMATEX, with the MS ADPCM extension when format == 2. */
typedef struct ft_avi_audio {
    uint16_t format;            /* 1 = PCM, 2 = MS ADPCM */
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint16_t samples_per_block; /* MS ADPCM */
    uint16_t coef_count;
    int16_t coef[FT_AVI_MAX_COEFS][2];
} ft_avi_audio;

typedef struct ft_avi {
    FILE *f;
    uint32_t width, height;
    uint32_t video_fourcc;      /* biCompression, e.g. 'cvid' */
    uint32_t rate, scale;       /* video frames per second = rate / scale */
    bool has_audio;
    ft_avi_audio audio;
    ft_avi_chunk *video;
    uint32_t video_count, video_cap;
    ft_avi_chunk *audio_chunks;
    uint32_t audio_count, audio_cap;
} ft_avi;

bool ft_avi_open(ft_avi *a, const char *path);
void ft_avi_close(ft_avi *a);

/* Reads a chunk into `buf` (at least c->size bytes). */
bool ft_avi_read(ft_avi *a, const ft_avi_chunk *c, uint8_t *buf);

/* The largest chunk of either list, for sizing one reusable buffer. */
uint32_t ft_avi_max_chunk(const ft_avi *a);
