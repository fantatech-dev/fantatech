#include "vfs/iscab.h"

#include "codec/inflate.h"
#include "core/ft_common.h"
#include "core/md5.h"

#include <stdlib.h>
#include <string.h>

#define ISCAB_SIG 0x28635349u /* "ISc(" */
#define DESC_SIZE 0x57
#define CHUNK_OUT_MAX (1u << 20)

static uint64_t rd64(const uint8_t *p)
{
    return ft_rd32(p) | ((uint64_t)ft_rd32(p + 4) << 32);
}

/* A NUL-terminated string inside the header, or NULL if it runs off the end. */
static const char *hdr_str(const ft_iscab *c, size_t off)
{
    if (off >= c->hdr_len || !memchr(c->hdr + off, 0, c->hdr_len - off))
        return NULL;
    return (const char *)c->hdr + off;
}

bool ft_iscab_open(ft_iscab *c, uint8_t *hdr, size_t len)
{
    memset(c, 0, sizeof *c);
    c->hdr = hdr;
    c->hdr_len = len;
    if (len < 20 || ft_rd32(hdr) != ISCAB_SIG) {
        ft_set_error("data1.hdr: bad signature");
        return false;
    }
    c->version = ft_rd32(hdr + 4);
    size_t cd = ft_rd32(hdr + 12);
    if (cd + 0x30 > len) {
        ft_set_error("data1.hdr: cab descriptor out of range");
        return false;
    }
    c->table = cd + ft_rd32(hdr + cd + 0x0c);
    c->dir_count = ft_rd32(hdr + cd + 0x1c);
    c->file_count = ft_rd32(hdr + cd + 0x28);
    c->descs = c->table + ft_rd32(hdr + cd + 0x2c);
    if (c->table + ((size_t)c->dir_count + c->file_count) * 4 > len ||
        c->descs + (size_t)c->file_count * DESC_SIZE > len) {
        ft_set_error("data1.hdr: file table out of range");
        return false;
    }
    return true;
}

void ft_iscab_close(ft_iscab *c)
{
    free(c->hdr);
    memset(c, 0, sizeof *c);
}

bool ft_iscab_file_at(const ft_iscab *c, uint32_t index, ft_iscab_file *out)
{
    if (index >= c->file_count)
        return false;
    const uint8_t *p = c->hdr + c->descs + (size_t)index * DESC_SIZE;
    out->flags = ft_rd16(p);
    out->expanded = rd64(p + 2);
    out->compressed = rd64(p + 10);
    out->data_offset = rd64(p + 18);
    memcpy(out->md5, p + 26, 16);
    out->name = hdr_str(c, c->table + ft_rd32(p + 58));
    uint16_t dir = ft_rd16(p + 62);
    out->dir = dir < c->dir_count ? hdr_str(c, c->table + ft_rd32(c->hdr + c->table + (size_t)dir * 4)) : NULL;
    out->link_flags = p[84];
    out->volume = ft_rd16(p + 85);
    return out->name && out->dir;
}

/* The stored bytes of one file, across volumes. */
typedef struct reader {
    const ft_iscab_file *file;
    uint32_t index;
    ft_iscab_open_fn open;
    void *user;
    FILE *f;
    unsigned volume;
    uint64_t pos, left;    /* absolute position in f, stored bytes left in this volume */
    bool synced;           /* f is already at pos (reads are sequential: skip the seek) */
    uint32_t seed;         /* de-obfuscation counter */
} reader;

static bool reader_open(reader *r, bool first)
{
    uint64_t base, size;
    if (!r->open(r->user, r->volume, &r->f, &base, &size))
        return false;
    uint8_t vh[20 + 0x40];
    if (size < sizeof vh || !ft_fseek64(r->f, base) || fread(vh, 1, sizeof vh, r->f) != sizeof vh ||
        ft_rd32(vh) != ISCAB_SIG) {
        ft_set_error("data%u.cab: bad volume header", r->volume);
        return false;
    }
    const uint8_t *v = vh + 20;
    uint32_t first_index = ft_rd32(v + 8), last_index = ft_rd32(v + 12);
    uint64_t first_offset = rd64(v + 16), first_compressed = rd64(v + 32);
    uint64_t last_offset = rd64(v + 40), last_compressed = rd64(v + 56);
    uint64_t offset, left;
    if (first) {
        if (r->index == last_index && last_offset != 0x7FFFFFFF && (r->file->flags & FT_ISCAB_SPLIT)) {
            offset = last_offset;
            left = last_compressed;
        } else {
            offset = r->file->data_offset;
            left = r->file->compressed;
        }
    } else {
        if (r->index != first_index) {
            ft_set_error("data%u.cab does not continue file %u", r->volume, r->index);
            return false;
        }
        offset = first_offset;
        left = first_compressed;
    }
    if (offset > size || left > size - offset) {
        ft_set_error("data%u.cab: file %u out of range", r->volume, r->index);
        return false;
    }
    r->pos = base + offset;
    r->left = left;
    r->synced = false;
    return true;
}

static bool reader_read(reader *r, uint8_t *buf, size_t n)
{
    uint8_t *start = buf;
    size_t total = n;
    while (n) {
        if (r->left == 0) {
            r->volume++;
            if (!reader_open(r, false))
                return false;
            continue;
        }
        size_t take = r->left < n ? (size_t)r->left : n;
        if ((!r->synced && !ft_fseek64(r->f, r->pos)) || fread(buf, 1, take, r->f) != take) {
            ft_set_error("data%u.cab: short read", r->volume);
            return false;
        }
        r->synced = true;
        r->pos += take;
        r->left -= take;
        buf += take;
        n -= take;
    }
    if (r->file->flags & FT_ISCAB_OBFUSCATED)
        for (size_t i = 0; i < total; i++) {
            uint8_t b = (uint8_t)(start[i] ^ 0xD5);
            b = (uint8_t)((b >> 2) | (b << 6));
            start[i] = (uint8_t)(b - (r->seed % 0x47));
            r->seed++;
        }
    return true;
}

bool ft_iscab_extract(const ft_iscab *c, uint32_t index, ft_iscab_open_fn open, void *open_user, ft_iscab_sink_fn sink,
                      void *sink_user)
{
    ft_iscab_file file;
    if (!ft_iscab_file_at(c, index, &file)) {
        ft_set_error("cab file %u: bad descriptor", index);
        return false;
    }
    reader r = { .file = &file, .index = index, .open = open, .user = open_user, .volume = file.volume };
    if (!reader_open(&r, true))
        return false;
    ft_md5 md5;
    ft_md5_init(&md5);
    uint64_t remaining = file.expanded;
    bool ok = true;
    if (file.flags & FT_ISCAB_COMPRESSED) {
        uint8_t *in = malloc(0x10000), *out = malloc(CHUNK_OUT_MAX);
        ok = in && out;
        if (!ok)
            ft_set_error("out of memory");
        uint64_t consumed = 0;
        while (ok && remaining && consumed < file.compressed) {
            uint8_t lb[2];
            size_t got = 0;
            ok = reader_read(&r, lb, 2) && reader_read(&r, in, ft_rd16(lb)) &&
                 ft_inflate_raw(in, ft_rd16(lb), out, CHUNK_OUT_MAX, &got);
            if (!ok)
                break;
            consumed += 2u + ft_rd16(lb);
            if (got > remaining)
                got = (size_t)remaining;
            ft_md5_update(&md5, out, got);
            ok = sink(sink_user, out, got);
            remaining -= got;
        }
        free(in);
        free(out);
    } else {
        uint8_t *buf = malloc(1u << 20);
        ok = buf != NULL;
        while (ok && remaining) {
            size_t take = remaining < (1u << 20) ? (size_t)remaining : (1u << 20);
            ok = reader_read(&r, buf, take);
            if (!ok)
                break;
            ft_md5_update(&md5, buf, take);
            ok = sink(sink_user, buf, take);
            remaining -= take;
        }
        free(buf);
    }
    if (!ok)
        return false;
    if (remaining) {
        ft_set_error("%s: %llu bytes short", file.name, (unsigned long long)remaining);
        return false;
    }
    uint8_t digest[16];
    ft_md5_final(&md5, digest);
    if (memcmp(digest, file.md5, 16) != 0) {
        ft_set_error("%s: MD5 mismatch", file.name);
        return false;
    }
    return true;
}
