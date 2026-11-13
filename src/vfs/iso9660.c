#include "vfs/iso9660.h"

#include "core/ft_common.h"

#include <stdlib.h>
#include <string.h>

#define SECTOR 2048u

static bool read_at(FILE *f, uint64_t offset, void *buf, size_t len)
{
    return ft_fseek64(f, offset) && fread(buf, 1, len, f) == len;
}

/* Compares a directory record name ("DATA1.HDR;1") with one path component. */
static bool name_matches(const uint8_t *name, size_t len, const char *want, size_t want_len)
{
    const uint8_t *semi = memchr(name, ';', len);
    if (semi)
        len = (size_t)(semi - name);
    if (len && name[len - 1] == '.')
        len--;
    if (len != want_len)
        return false;
    for (size_t i = 0; i < len; i++) {
        int a = name[i], b = (unsigned char)want[i];
        if (a >= 'a' && a <= 'z')
            a -= 'a' - 'A';
        if (b >= 'a' && b <= 'z')
            b -= 'a' - 'A';
        if (a != b)
            return false;
    }
    return true;
}

/* Looks `want` up in the directory at `extent`/`size`; returns the matching record's fields. */
static bool dir_find(FILE *f, uint32_t extent, uint32_t size, const char *want, size_t want_len, uint32_t *out_extent,
                     uint32_t *out_size, bool *out_dir)
{
    if (size > 64u << 20) {
        ft_set_error("ISO: directory too large");
        return false;
    }
    uint8_t *buf = malloc(size ? size : 1);
    if (!buf || !read_at(f, (uint64_t)extent * SECTOR, buf, size)) {
        free(buf);
        ft_set_error("ISO: cannot read a directory");
        return false;
    }
    bool found = false;
    for (uint32_t pos = 0; pos < size;) {
        uint8_t len = buf[pos];
        if (len == 0) { /* records don't cross sectors: skip to the next one */
            pos = (pos / SECTOR + 1) * SECTOR;
            continue;
        }
        if (len < 34 || pos + len > size)
            break;
        const uint8_t *r = buf + pos;
        uint8_t name_len = r[32];
        if (33u + name_len <= len && name_matches(r + 33, name_len, want, want_len)) {
            *out_extent = ft_rd32(r + 2);
            *out_size = ft_rd32(r + 10);
            *out_dir = (r[25] & 2) != 0;
            found = true;
            break;
        }
        pos += len;
    }
    free(buf);
    return found;
}

bool ft_iso_find(FILE *iso, const char *path, uint64_t *offset, uint64_t *size)
{
    uint8_t pvd[SECTOR];
    if (!read_at(iso, 16 * SECTOR, pvd, sizeof pvd) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        ft_set_error("not an ISO 9660 image (no primary volume descriptor)");
        return false;
    }
    const uint8_t *root = pvd + 156;
    uint32_t extent = ft_rd32(root + 2), dsize = ft_rd32(root + 10);
    bool is_dir = true;
    const char *p = path;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (!is_dir || !dir_find(iso, extent, dsize, p, n, &extent, &dsize, &is_dir)) {
            ft_set_error("%s not found in the disc image", path);
            return false;
        }
        p += n;
        if (*p == '/')
            p++;
    }
    if (is_dir) {
        ft_set_error("%s is a directory", path);
        return false;
    }
    *offset = (uint64_t)extent * SECTOR;
    *size = dsize;
    return true;
}
