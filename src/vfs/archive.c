#include "vfs/archive.h"

#include <stdlib.h>
#include <string.h>

#define FIXED_RECORD_SIZE 24
#define FIXED_NAME_LEN 15

const char *const ft_archive_names[] = {
    "bg02", "eve1024", "eve2048", "event", "face", "face1024", "face512", "film1024", "film2048",
    "frame", "music", "se", "stafroll", "text", "text3d", "textbmp", "voice",
};
const size_t ft_archive_name_count = sizeof ft_archive_names / sizeof ft_archive_names[0];

static bool parse_fixed(ft_archive *a, const uint8_t *idx)
{
    for (uint32_t i = 0; i < a->count; i++) {
        const uint8_t *r = idx + 4 + (size_t)i * FIXED_RECORD_SIZE;
        ft_entry *e = &a->entries[i];
        e->offset = ft_rd32(r);
        e->size = ft_rd32(r + 4);
        if (r[8] != 0)
            a->nonzero_flags++;
        memcpy(e->name, r + 9, FIXED_NAME_LEN);
        e->name[FIXED_NAME_LEN] = 0;
    }
    return true;
}

static bool parse_variable(ft_archive *a, const uint8_t *idx, size_t len)
{
    size_t pos = 4;
    for (uint32_t i = 0; i < a->count; i++) {
        if (len - pos < 4)
            goto truncated;
        uint32_t name_len = ft_rd32(idx + pos);
        pos += 4;
        if (name_len >= FT_ENTRY_NAME_MAX) {
            ft_set_error("%s.idx: record %u name length %u too long", a->name, i, name_len);
            return false;
        }
        if (len - pos < (size_t)name_len + 8)
            goto truncated;
        ft_entry *e = &a->entries[i];
        memcpy(e->name, idx + pos, name_len);
        e->name[name_len] = 0;
        pos += name_len;
        e->offset = ft_rd32(idx + pos);
        e->size = ft_rd32(idx + pos + 4);
        pos += 8;
    }
    if (pos != len) {
        ft_set_error("%s.idx: %zu trailing bytes after %u variable records", a->name, len - pos, a->count);
        return false;
    }
    return true;

truncated:
    ft_set_error("%s.idx: variable records run past the end", a->name);
    return false;
}

bool ft_archive_open(ft_archive *a, const char *dir, const char *name)
{
    memset(a, 0, sizeof *a);
    snprintf(a->name, sizeof a->name, "%s", name);

    char path[1024];
    snprintf(path, sizeof path, "%s/%s.idx", dir, name);
    size_t len;
    uint8_t *idx = ft_read_file(path, &len);
    if (!idx)
        return false;
    if (len < 4) {
        ft_set_error("%s: too short", path);
        goto fail;
    }

    a->count = ft_rd32(idx);
    a->variable_layout = (uint64_t)a->count * FIXED_RECORD_SIZE + 4 != len;
    if (a->count > (len - 4) / 9) {  /* smallest possible record is 9 bytes (variable, 1-char name) */
        ft_set_error("%s: implausible record count %u", path, a->count);
        goto fail;
    }
    a->entries = calloc(a->count ? a->count : 1, sizeof *a->entries);
    if (!a->entries) {
        ft_set_error("out of memory");
        goto fail;
    }
    if (!(a->variable_layout ? parse_variable(a, idx, len) : parse_fixed(a, idx)))
        goto fail;
    free(idx);
    idx = NULL;

    snprintf(path, sizeof path, "%s/%s.%s", dir, name, a->variable_layout ? "bin" : "lb5");
    a->payload = ft_fopen(path, "rb");
    if (!a->payload) {
        ft_set_error("cannot open %s", path);
        goto fail;
    }
    if (!ft_fsize64(a->payload, &a->payload_size)) {
        ft_set_error("cannot size %s", path);
        goto fail;
    }
    for (uint32_t i = 0; i < a->count; i++) {
        uint64_t end = (uint64_t)a->entries[i].offset + a->entries[i].size;
        if (end > a->payload_size) {
            ft_set_error("%s: entry %s ends at %llu, past payload size %llu", a->name, a->entries[i].name,
                         (unsigned long long)end, (unsigned long long)a->payload_size);
            goto fail;
        }
        if (end > a->max_end)
            a->max_end = end;
    }
    return true;

fail:
    free(idx);
    ft_archive_close(a);
    return false;
}

void ft_archive_close(ft_archive *a)
{
    if (a->payload)
        fclose(a->payload);
    free(a->entries);
    memset(a, 0, sizeof *a);
}

const ft_entry *ft_archive_find(const ft_archive *a, const char *name)
{
    for (uint32_t i = 0; i < a->count; i++)
        if (ft_stricmp(a->entries[i].name, name) == 0)
            return &a->entries[i];
    return NULL;
}

uint8_t *ft_archive_read(const ft_archive *a, const ft_entry *e)
{
    uint8_t *buf = malloc((size_t)e->size + 1);
    if (!buf) {
        ft_set_error("out of memory reading %s/%s", a->name, e->name);
        return NULL;
    }
    if (!ft_fseek64(a->payload, e->offset) || fread(buf, 1, e->size, a->payload) != e->size) {
        ft_set_error("read failed for %s/%s", a->name, e->name);
        free(buf);
        return NULL;
    }
    buf[e->size] = 0;
    return buf;
}
