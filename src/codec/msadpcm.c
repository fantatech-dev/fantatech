#include "codec/msadpcm.h"

static const int adaptation[16] = { 230, 230, 230, 230, 307, 409, 512, 614,
                                    768, 614, 512, 409, 307, 230, 230, 230 };

typedef struct channel {
    int c1, c2;                 /* coefficients, x256 */
    int delta;
    int s1, s2;                 /* the last two samples */
} channel;

static int16_t clamp16(int v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static int16_t expand(channel *c, int nibble)
{
    int predicted = (c->s1 * c->c1 + c->s2 * c->c2) / 256;
    predicted += ((nibble & 0x08) ? nibble - 16 : nibble) * c->delta;
    c->s2 = c->s1;
    c->s1 = clamp16(predicted);
    c->delta = (adaptation[nibble] * c->delta) >> 8;
    if (c->delta < 16)
        c->delta = 16;
    if (c->delta > 0x7fffffff / 768) /* runaway step on bad data */
        c->delta = 0x7fffffff / 768;
    return (int16_t)c->s1;
}

uint32_t ft_msadpcm_block_samples(const ft_avi_audio *fmt, size_t len)
{
    size_t ch = fmt->channels;
    if (ch < 1 || ch > 2 || len < 7 * ch)
        return 0;
    if (fmt->block_align && len > fmt->block_align)
        len = fmt->block_align;
    return (uint32_t)((len - 6 * ch) * 2 / ch); /* 2 header samples + 2 (mono) or 1 (stereo) a byte */
}

/* One block; returns its samples per channel. */
static uint32_t decode_block(const ft_avi_audio *fmt, const uint8_t *p, size_t len, int16_t *out)
{
    int ch = fmt->channels;
    uint32_t n = ft_msadpcm_block_samples(fmt, len);
    if (n < 2)
        return 0;
    channel c[2];
    for (int i = 0; i < ch; i++) {
        int pred = p[i];
        if (pred >= fmt->coef_count)
            return 0;
        c[i].c1 = fmt->coef[pred][0];
        c[i].c2 = fmt->coef[pred][1];
    }
    p += ch;
    for (int i = 0; i < ch; i++, p += 2)
        c[i].delta = (int16_t)ft_rd16(p);
    for (int i = 0; i < ch; i++, p += 2)
        c[i].s1 = (int16_t)ft_rd16(p);
    for (int i = 0; i < ch; i++, p += 2)
        c[i].s2 = (int16_t)ft_rd16(p);
    for (int i = 0; i < ch; i++) /* the header's samples, oldest first */
        *out++ = (int16_t)c[i].s2;
    for (int i = 0; i < ch; i++)
        *out++ = (int16_t)c[i].s1;
    for (uint32_t bytes = ch == 2 ? n - 2 : (n - 2) / 2; bytes > 0; bytes--) {
        int b = *p++;
        *out++ = expand(&c[0], b >> 4);
        *out++ = expand(&c[ch - 1], b & 0x0f);
    }
    return n;
}

uint32_t ft_msadpcm_decode(const ft_avi_audio *fmt, const uint8_t *data, size_t len, int16_t *out)
{
    if (fmt->format != 2 || fmt->block_align == 0 || fmt->channels < 1 || fmt->channels > 2) {
        ft_set_error("not MS ADPCM");
        return 0;
    }
    uint32_t total = 0;
    while (len >= 7u * fmt->channels) {
        size_t block = len < fmt->block_align ? len : fmt->block_align;
        uint32_t n = decode_block(fmt, data, block, out);
        if (n == 0) {
            ft_set_error("MS ADPCM: bad block");
            return 0;
        }
        out += (size_t)n * fmt->channels;
        total += n;
        data += block;
        len -= block;
    }
    return total;
}
