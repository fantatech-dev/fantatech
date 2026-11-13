#include "core/md5.h"

#include "core/ft_common.h"

#include <string.h>

static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t S[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

static uint32_t rol(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void transform(ft_md5 *m, const uint8_t *p)
{
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
        w[i] = ft_rd32(p + i * 4);
    uint32_t a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        switch (i >> 4) {
        case 0: f = (b & c) | (~b & d); g = i; break;
        case 1: f = (d & b) | (~d & c); g = (5 * i + 1) & 15; break;
        case 2: f = b ^ c ^ d; g = (3 * i + 5) & 15; break;
        default: f = c ^ (b | ~d); g = (7 * i) & 15; break;
        }
        uint32_t t = d;
        d = c;
        c = b;
        b = b + rol(a + f + K[i] + w[g], S[(i >> 4) * 4 + (i & 3)]);
        a = t;
    }
    m->h[0] += a;
    m->h[1] += b;
    m->h[2] += c;
    m->h[3] += d;
}

void ft_md5_init(ft_md5 *m)
{
    m->h[0] = 0x67452301;
    m->h[1] = 0xefcdab89;
    m->h[2] = 0x98badcfe;
    m->h[3] = 0x10325476;
    m->length = 0;
    m->fill = 0;
}

void ft_md5_update(ft_md5 *m, const void *data, size_t len)
{
    const uint8_t *p = data;
    m->length += len;
    if (m->fill) {
        size_t take = 64 - m->fill < len ? 64 - m->fill : len;
        memcpy(m->block + m->fill, p, take);
        m->fill += take;
        p += take;
        len -= take;
        if (m->fill < 64)
            return;
        transform(m, m->block);
        m->fill = 0;
    }
    for (; len >= 64; p += 64, len -= 64)
        transform(m, p);
    memcpy(m->block, p, len);
    m->fill = len;
}

void ft_md5_final(ft_md5 *m, uint8_t digest[16])
{
    uint64_t bits = m->length * 8;
    uint8_t pad[72] = { 0x80 };
    size_t n = m->fill < 56 ? 56 - m->fill : 120 - m->fill;
    for (int i = 0; i < 8; i++)
        pad[n + i] = (uint8_t)(bits >> (8 * i));
    ft_md5_update(m, pad, n + 8);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            digest[i * 4 + j] = (uint8_t)(m->h[i] >> (8 * j));
}
