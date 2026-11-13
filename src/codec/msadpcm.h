/*
 * Microsoft ADPCM (WAVE format 2), the intro movie's audio: blocks of `block_align` bytes, each
 * starting with a predictor index, a step size and two samples per channel, then 4-bit codes
 * (high nibble first; in stereo the high one is the left channel). Prediction is
 * (s1 * c1 + s2 * c2) / 256 with C's truncating division, like msadp32.acm and ffmpeg.
 */
#pragma once

#include "codec/avi.h"

/* Samples per channel a block of `len` bytes holds (a short last block holds fewer). */
uint32_t ft_msadpcm_block_samples(const ft_avi_audio *fmt, size_t len);

/*
 * Decodes the blocks in `data` (whole blocks plus a possibly short last one) into interleaved
 * 16-bit samples. `out` needs room for the sum of ft_msadpcm_block_samples() over the blocks,
 * times the channel count. Returns the samples per channel written, or 0 on bad data.
 */
uint32_t ft_msadpcm_decode(const ft_avi_audio *fmt, const uint8_t *data, size_t len, int16_t *out);
