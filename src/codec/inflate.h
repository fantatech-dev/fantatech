/*
 * Raw DEFLATE (RFC 1951) decoder, enough for the InstallShield cabinet chunks the
 * installer unpacks. No zlib/gzip wrapper, whole input and output in memory.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Inflates `in` into `out` (at most `out_cap` bytes). Stops after the final block. On
 * success *out_len is the number of bytes written; on failure the error is set.
 */
bool ft_inflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);
