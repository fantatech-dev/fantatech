#include "codec/text.h"

#include "codec/cp932.h"

void ft_text_deobfuscate(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (buf[i] > 15)
            buf[i] = (uint8_t)(14 - buf[i]);
}

char *ft_text_decode(uint8_t *raw, size_t len, size_t *utf8_len_out, size_t *bad_out)
{
    ft_text_deobfuscate(raw, len);
    return ft_cp932_to_utf8(raw, len, utf8_len_out, bad_out);
}
