/*
 * Script text (.txt entries in text.lb5 / text3d.lb5).
 *
 * mana01's kt_text_deobfuscate (0x453020) maps every byte b > 15 to (14 - b) & 0xFF in
 * place, leaving 0..15 (CR, LF, tab) alone. The result is CP932 text.
 */
#pragma once

#include "core/ft_common.h"

void ft_text_deobfuscate(uint8_t *buf, size_t len);

/* De-obfuscates a raw entry (in place) and returns it as malloc'd UTF-8. */
char *ft_text_decode(uint8_t *raw, size_t len, size_t *utf8_len_out, size_t *bad_out);
