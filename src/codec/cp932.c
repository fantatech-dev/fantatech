#include "codec/cp932.h"

#include <stdlib.h>

static int lead_index(uint8_t b)
{
    if (b >= 0x81 && b <= 0x9F)
        return b - 0x81;
    if (b >= 0xE0 && b <= 0xFC)
        return b - 0xE0 + 0x1F;
    return -1;
}

static size_t put_utf8(char *out, uint32_t cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

char *ft_cp932_to_utf8(const uint8_t *src, size_t len, size_t *utf8_len_out, size_t *bad_out)
{
    /* Every input byte becomes at most 3 output bytes (BMP only). */
    char *out = malloc(len * 3 + 1);
    if (!out) {
        ft_set_error("out of memory");
        return NULL;
    }
    size_t o = 0, bad = 0;
    for (size_t i = 0; i < len;) {
        uint8_t b = src[i];
        int li = lead_index(b);
        uint32_t cp;
        if (li < 0) {
            cp = ft_cp932_single[b];
            if (cp == 0 && b != 0)
                cp = 0xFFFD, bad++;
            i++;
        } else if (i + 1 < len && src[i + 1] >= 0x40 && src[i + 1] <= 0xFC) {
            cp = ft_cp932_double[li][src[i + 1] - 0x40];
            if (cp == 0)
                cp = 0xFFFD, bad++;
            i += 2;
        } else {
            cp = 0xFFFD, bad++;
            i++;
        }
        o += put_utf8(out + o, cp);
    }
    out[o] = 0;
    if (utf8_len_out)
        *utf8_len_out = o;
    if (bad_out)
        *bad_out = bad;
    return out;
}
