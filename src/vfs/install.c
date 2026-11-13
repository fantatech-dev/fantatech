#include "vfs/install.h"

#include "core/ft_common.h"
#include "vfs/iscab.h"
#include "vfs/iso9660.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VOLUMES 16
#define FIRST_GAME_VOLUME 2

typedef struct installer {
    FILE *iso;
    uint64_t vol_offset[MAX_VOLUMES], vol_size[MAX_VOLUMES];
    bool vol_found[MAX_VOLUMES];
    /* the file being written */
    FILE *out;
    const char *name;
    uint64_t done, total;
    ft_install_progress_fn progress;
    void *user;
} installer;

static bool open_volume(void *user, unsigned volume, FILE **f, uint64_t *base, uint64_t *size)
{
    installer *in = user;
    if (volume >= MAX_VOLUMES) {
        ft_set_error("cabinet volume %u out of range", volume);
        return false;
    }
    if (!in->vol_found[volume]) {
        char name[32];
        snprintf(name, sizeof name, "data%u.cab", volume);
        if (!ft_iso_find(in->iso, name, &in->vol_offset[volume], &in->vol_size[volume]))
            return false;
        in->vol_found[volume] = true;
    }
    *f = in->iso;
    *base = in->vol_offset[volume];
    *size = in->vol_size[volume];
    return true;
}

static bool write_out(void *user, const uint8_t *data, size_t len)
{
    installer *in = user;
    if (fwrite(data, 1, len, in->out) != len) {
        ft_set_error("write failed (disk full?)");
        return false;
    }
    in->done += len;
    if (in->progress)
        in->progress(in->user, in->name, in->done, in->total);
    return true;
}

/* Cabinet directory names are relative Windows paths; refuse anything that could escape. */
static bool safe_component(const char *s)
{
    if (s[0] == '\\' || s[0] == '/' || strchr(s, ':') || strstr(s, ".."))
        return false;
    return true;
}

static bool wanted(const ft_iscab_file *f)
{
    return !(f->flags & FT_ISCAB_INVALID) && !(f->link_flags & 1) && f->name[0] && f->volume >= FIRST_GAME_VOLUME;
}

static bool install_file(installer *in, const ft_iscab *cab, uint32_t index, const ft_iscab_file *f,
                         const char *out_dir)
{
    if (!safe_component(f->dir) || !safe_component(f->name)) {
        ft_set_error("unsafe path in the cabinet: %s\\%s", f->dir, f->name);
        return false;
    }
    char dir[1024], path[1100], part[1110];
    snprintf(dir, sizeof dir, "%s%s%s", out_dir, f->dir[0] ? "/" : "", f->dir);
    for (char *p = dir; *p; p++)
        if (*p == '\\')
            *p = '/';
    snprintf(path, sizeof path, "%s/%s", dir, f->name);
    snprintf(part, sizeof part, "%s.part", path);
    if (!ft_mkdirs(dir))
        return false;
    in->out = ft_fopen(part, "wb");
    if (!in->out) {
        ft_set_error("cannot create %s", part);
        return false;
    }
    in->name = f->name;
    bool ok = ft_iscab_extract(cab, index, open_volume, in, write_out, in);
    ok = (fclose(in->out) == 0) && ok;
    in->out = NULL;
    if (ok) {
        ft_remove(path);
        ok = ft_rename(part, path) == 0;
        if (!ok)
            ft_set_error("cannot rename %s", part);
    }
    if (!ok)
        ft_remove(part);
    return ok;
}

bool ft_install_from_iso(const char *iso_path, const char *out_dir, ft_install_progress_fn progress, void *user)
{
    installer in = { .progress = progress, .user = user };
    in.iso = ft_fopen(iso_path, "rb");
    if (!in.iso) {
        ft_set_error("cannot open %s", iso_path);
        return false;
    }
    bool ok = false;
    ft_iscab cab = { 0 };
    uint64_t hdr_off, hdr_size;
    uint8_t *hdr = NULL;
    if (!ft_iso_find(in.iso, "data1.hdr", &hdr_off, &hdr_size))
        goto done;
    if (hdr_size > (16u << 20) || !(hdr = malloc((size_t)hdr_size)) || !ft_fseek64(in.iso, hdr_off) ||
        fread(hdr, 1, (size_t)hdr_size, in.iso) != hdr_size) {
        free(hdr);
        ft_set_error("cannot read data1.hdr from the disc image");
        goto done;
    }
    if (!ft_iscab_open(&cab, hdr, (size_t)hdr_size))
        goto done;
    ft_iscab_file f;
    for (uint32_t i = 0; i < cab.file_count; i++)
        if (ft_iscab_file_at(&cab, i, &f) && wanted(&f))
            in.total += f.expanded;
    if (in.total == 0) {
        ft_set_error("no game files in the cabinet");
        goto done;
    }
    for (uint32_t i = 0; i < cab.file_count; i++) {
        if (!ft_iscab_file_at(&cab, i, &f) || !wanted(&f))
            continue;
        if (!install_file(&in, &cab, i, &f, out_dir))
            goto done;
    }
    ok = true;
done:
    ft_iscab_close(&cab);
    fclose(in.iso);
    return ok;
}
