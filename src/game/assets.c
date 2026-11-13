#include "game/assets.h"

#include "codec/bmp.h"
#include "codec/text.h"

#include <stdlib.h>
#include <string.h>

bool ft_assets_open(ft_assets *a, const char *dir)
{
    memset(a, 0, sizeof *a);
    snprintf(a->dir, sizeof a->dir, "%s", dir);
    for (int i = 0; i < FT_ARC_COUNT; i++)
        if (!ft_archive_open(&a->arc[i], dir, ft_archive_names[i])) {
            ft_assets_close(a);
            return false;
        }

    uint32_t total = a->arc[FT_ARC_TEXT].count + a->arc[FT_ARC_TEXT3D].count;
    a->scripts = calloc(total ? total : 1, sizeof *a->scripts);
    if (!a->scripts) {
        ft_set_error("out of memory");
        ft_assets_close(a);
        return false;
    }
    static const ft_arc text_arcs[] = { FT_ARC_TEXT, FT_ARC_TEXT3D };
    for (int k = 0; k < 2; k++) {
        ft_archive *arc = &a->arc[text_arcs[k]];
        for (uint32_t i = 0; i < arc->count; i++) {
            uint8_t *raw = ft_archive_read(arc, &arc->entries[i]);
            size_t len = 0;
            char *text = raw ? ft_text_decode(raw, arc->entries[i].size, &len, NULL) : NULL;
            free(raw);
            char stem[FT_ENTRY_NAME_MAX];
            snprintf(stem, sizeof stem, "%s", arc->entries[i].name);
            char *dot = strrchr(stem, '.');
            if (dot)
                *dot = 0;
            bool ok = text && ft_script_parse(&a->scripts[a->script_count], stem, text, len);
            free(text);
            if (!ok) {
                ft_assets_close(a);
                return false;
            }
            a->script_count++;
        }
    }
    return true;
}

void ft_assets_close(ft_assets *a)
{
    for (int i = 0; i < FT_ARC_COUNT; i++)
        if (a->arc[i].entries)
            ft_archive_close(&a->arc[i]);
    for (uint32_t i = 0; i < a->script_count; i++)
        ft_script_free(&a->scripts[i]);
    free(a->scripts);
    memset(a, 0, sizeof *a);
}

const ft_script *ft_assets_script(const ft_assets *a, const char *name)
{
    for (uint32_t i = 0; i < a->script_count; i++)
        if (ft_stricmp(a->scripts[i].name, name) == 0)
            return &a->scripts[i];
    return NULL;
}

bool ft_assets_read(ft_assets *a, ft_arc arc, const char *name, ft_blob *out)
{
    out->data = NULL;
    out->size = 0;
    out->arc = arc;
    const ft_entry *e = ft_archive_find(&a->arc[arc], name);
    if (!e)
        return false;
    out->data = ft_archive_read(&a->arc[arc], e);
    out->size = e->size;
    out->arc = arc;
    return out->data != NULL;
}

float ft_assets_scale(ft_arc arc)
{
    switch (arc) {
    case FT_ARC_EVENT: return 1024.0f / 740.0f;
    case FT_ARC_EVE2048:
    case FT_ARC_FILM2048: return 1024.0f / 1894.0f;
    default: return 1.0f;
    }
}

/* Tries code + each extension in each archive, in order. */
static bool find_any(ft_assets *a, const ft_arc *arcs, int n, const char *code, ft_blob *out)
{
    static const char *const exts[] = { ".bmp", ".zdf" };
    char name[FT_ENTRY_NAME_MAX];
    for (int i = 0; i < n; i++)
        for (int e = 0; e < 2; e++) {
            snprintf(name, sizeof name, "%s%s", code, exts[e]);
            if (ft_assets_read(a, arcs[i], name, out))
                return true;
        }
    return false;
}

bool ft_assets_face(ft_assets *a, const char *code, ft_blob *out)
{
    static const ft_arc order[] = { FT_ARC_FACE512, FT_ARC_FACE, FT_ARC_FACE1024 };
    return find_any(a, order, 3, code, out);
}

bool ft_assets_cg(ft_assets *a, const char *code, ft_blob *out)
{
    static const ft_arc order[] = { FT_ARC_EVE1024, FT_ARC_EVENT, FT_ARC_EVE2048, FT_ARC_BG02, FT_ARC_FILM1024 };
    return find_any(a, order, 5, code, out);
}

bool ft_assets_bg02(ft_assets *a, const char *code, ft_blob *out)
{
    static const ft_arc order[] = { FT_ARC_BG02, FT_ARC_FRAME };
    return find_any(a, order, 2, code, out) || ft_assets_cg(a, code, out);
}

bool ft_assets_film(ft_assets *a, const char *code, ft_blob *out)
{
    static const ft_arc order[] = { FT_ARC_FILM1024, FT_ARC_EVENT, FT_ARC_FILM2048 };
    return find_any(a, order, 3, code, out) || ft_assets_cg(a, code, out);
}

bool ft_assets_textline(ft_assets *a, const char *id, ft_blob *out)
{
    static const ft_arc order[] = { FT_ARC_TEXTBMP };
    return find_any(a, order, 1, id, out);
}

bool ft_assets_decode(const ft_blob *b, ft_image *out)
{
    if (ft_bmp_is(b->data, b->size))
        return ft_bmp_decode(b->data, b->size, out, NULL);
    if (ft_zdf_is(b->data, b->size)) {
        ft_zdf z;
        if (!ft_zdf_open(&z, b->data, b->size))
            return false;
        bool ok = ft_zdf_next(&z, out);
        ft_zdf_close(&z);
        return ok;
    }
    ft_set_error("unknown image format");
    return false;
}

bool ft_assets_sound(ft_assets *a, const char *code, ft_blob *out)
{
    char name[FT_ENTRY_NAME_MAX];
    snprintf(name, sizeof name, "%s.wav", code);
    return ft_assets_read(a, FT_ARC_VOICE, name, out) || ft_assets_read(a, FT_ARC_SE, name, out);
}

bool ft_assets_music(ft_assets *a, const char *stem, ft_blob *out)
{
    char name[FT_ENTRY_NAME_MAX];
    snprintf(name, sizeof name, "%s.wav", stem);
    return ft_assets_read(a, FT_ARC_MUSIC, name, out);
}
