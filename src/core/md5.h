/* MD5 (RFC 1321), for checking installed files against the cabinet's stored digests. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct ft_md5 {
    uint32_t h[4];
    uint64_t length;  /* bytes so far */
    uint8_t block[64];
    size_t fill;
} ft_md5;

void ft_md5_init(ft_md5 *m);
void ft_md5_update(ft_md5 *m, const void *data, size_t len);
void ft_md5_final(ft_md5 *m, uint8_t digest[16]);
