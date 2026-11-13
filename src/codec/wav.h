/*
 * RIFF WAVE entries (music.bin, se.bin, voice.bin). Only uncompressed PCM is expected.
 */
#pragma once

#include "core/ft_common.h"

typedef struct ft_wav {
    uint16_t format;        /* 1 = PCM */
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    const uint8_t *data;    /* points into the parsed buffer */
    uint32_t data_size;
} ft_wav;

/* Parses a WAVE file in memory. `data` stays owned by the caller's buffer. */
bool ft_wav_parse(const uint8_t *buf, size_t len, ft_wav *out);
