#include "codec/avi.h"

#include <stdlib.h>
#include <string.h>

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static bool read_at(FILE *f, uint64_t off, void *buf, size_t len)
{
    return ft_fseek64(f, off) && fread(buf, 1, len, f) == len;
}

static uint64_t min64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static bool push(ft_avi_chunk **list, uint32_t *count, uint32_t *cap, uint64_t offset, uint32_t size)
{
    if (*count == *cap) {
        uint32_t n = *cap ? *cap * 2 : 256;
        ft_avi_chunk *grown = realloc(*list, sizeof **list * n);
        if (!grown)
            return false;
        *list = grown;
        *cap = n;
    }
    (*list)[(*count)++] = (ft_avi_chunk){ offset, size };
    return true;
}

/*
 * The stream lists inside hdrl, in order: strh says what the stream is, strf how it's coded. A
 * chunk in movi is named by its stream number ("00dc", "01wb").
 */
typedef struct stream_kinds {
    int video, audio;           /* stream numbers, or -1 */
} stream_kinds;

static bool parse_strl(ft_avi *a, const uint8_t *p, uint32_t len, int index, stream_kinds *k)
{
    uint32_t type = 0;
    for (uint32_t o = 0; o + 8 <= len;) {
        uint32_t id = ft_rd32(p + o), size = ft_rd32(p + o + 4);
        const uint8_t *d = p + o + 8;
        if (size > len - o - 8)
            break;
        if (id == FOURCC('s', 't', 'r', 'h') && size >= 32) {
            type = ft_rd32(d);
            if (type == FOURCC('v', 'i', 'd', 's') && k->video < 0) {
                a->scale = ft_rd32(d + 20);
                a->rate = ft_rd32(d + 24);
            }
        } else if (id == FOURCC('s', 't', 'r', 'f')) {
            if (type == FOURCC('v', 'i', 'd', 's') && k->video < 0 && size >= 40) { /* BITMAPINFOHEADER */
                a->width = ft_rd32(d + 4);
                a->height = ft_rd32(d + 8);
                a->video_fourcc = ft_rd32(d + 16);
                k->video = index;
            } else if (type == FOURCC('a', 'u', 'd', 's') && k->audio < 0 && size >= 16) { /* WAVEFORMATEX */
                ft_avi_audio *au = &a->audio;
                au->format = ft_rd16(d);
                au->channels = ft_rd16(d + 2);
                au->sample_rate = ft_rd32(d + 4);
                au->block_align = ft_rd16(d + 12);
                au->bits_per_sample = ft_rd16(d + 14);
                if (au->format == 2 && size >= 22) { /* cbSize, wSamplesPerBlock, wNumCoef, coefs */
                    au->samples_per_block = ft_rd16(d + 18);
                    au->coef_count = ft_rd16(d + 20);
                    if (au->coef_count > FT_AVI_MAX_COEFS || 22 + 4u * au->coef_count > size)
                        au->coef_count = 0;
                    for (int i = 0; i < au->coef_count; i++) {
                        au->coef[i][0] = (int16_t)ft_rd16(d + 22 + 4 * i);
                        au->coef[i][1] = (int16_t)ft_rd16(d + 24 + 4 * i);
                    }
                }
                k->audio = index;
                a->has_audio = true;
            }
        }
        o += 8 + size + (size & 1);
    }
    return true;
}

/* Walks a movi list (and any 'rec ' lists in it), noting each stream's chunks. */
static bool walk_movi(ft_avi *a, uint64_t off, uint64_t end, const stream_kinds *k)
{
    uint8_t h[12];
    while (off + 8 <= end) {
        if (!read_at(a->f, off, h, 8))
            return false;
        uint32_t id = ft_rd32(h), size = ft_rd32(h + 4);
        if (id == FOURCC('L', 'I', 'S', 'T')) {
            if (!read_at(a->f, off + 8, h + 8, 4))
                return false;
            if (ft_rd32(h + 8) == FOURCC('r', 'e', 'c', ' ') && !walk_movi(a, off + 12, off + 8 + size, k))
                return false;
        } else if (h[0] >= '0' && h[0] <= '9' && h[1] >= '0' && h[1] <= '9') {
            int stream = (h[0] - '0') * 10 + (h[1] - '0');
            bool ok = true;
            if (stream == k->video)
                ok = push(&a->video, &a->video_count, &a->video_cap, off + 8, size);
            else if (stream == k->audio)
                ok = push(&a->audio_chunks, &a->audio_count, &a->audio_cap, off + 8, size);
            if (!ok) {
                ft_set_error("out of memory");
                return false;
            }
        }
        off += 8 + (uint64_t)size + (size & 1);
    }
    return true;
}

bool ft_avi_open(ft_avi *a, const char *path)
{
    memset(a, 0, sizeof *a);
    a->f = ft_fopen(path, "rb");
    if (!a->f) {
        ft_set_error("can't open %s", path);
        return false;
    }
    uint64_t file_size = 0;
    uint8_t h[12];
    if (!ft_fsize64(a->f, &file_size) || !read_at(a->f, 0, h, 12) || ft_rd32(h) != FOURCC('R', 'I', 'F', 'F') ||
        ft_rd32(h + 8) != FOURCC('A', 'V', 'I', ' ')) {
        ft_set_error("%s: not an AVI file", path);
        ft_avi_close(a);
        return false;
    }
    uint64_t end = min64(8 + (uint64_t)ft_rd32(h + 4), file_size);
    stream_kinds k = { -1, -1 };
    bool movi = false;
    for (uint64_t off = 12; off + 12 <= end;) {
        if (!read_at(a->f, off, h, 12))
            break;
        uint32_t id = ft_rd32(h), size = ft_rd32(h + 4), type = ft_rd32(h + 8);
        if (id == FOURCC('L', 'I', 'S', 'T') && type == FOURCC('h', 'd', 'r', 'l') && size >= 4) {
            uint8_t *hdrl = malloc(size - 4);
            if (!hdrl || !read_at(a->f, off + 12, hdrl, size - 4)) {
                free(hdrl);
                break;
            }
            int index = 0;
            for (uint32_t o = 0; o + 12 <= size - 4;) {
                uint32_t sid = ft_rd32(hdrl + o), ssize = ft_rd32(hdrl + o + 4);
                if (ssize > size - 4 - o - 8)
                    break;
                if (sid == FOURCC('L', 'I', 'S', 'T') && ft_rd32(hdrl + o + 8) == FOURCC('s', 't', 'r', 'l'))
                    parse_strl(a, hdrl + o + 12, ssize - 4, index++, &k);
                o += 8 + ssize + (ssize & 1);
            }
            free(hdrl);
        } else if (id == FOURCC('L', 'I', 'S', 'T') && type == FOURCC('m', 'o', 'v', 'i')) {
            movi = walk_movi(a, off + 12, min64(off + 8 + size, end), &k);
        }
        off += 8 + (uint64_t)size + (size & 1);
    }
    if (!movi || k.video < 0 || a->video_count == 0 || a->rate == 0 || a->scale == 0) {
        ft_set_error("%s: no video stream found", path);
        ft_avi_close(a);
        return false;
    }
    return true;
}

void ft_avi_close(ft_avi *a)
{
    if (a->f)
        fclose(a->f);
    free(a->video);
    free(a->audio_chunks);
    memset(a, 0, sizeof *a);
}

bool ft_avi_read(ft_avi *a, const ft_avi_chunk *c, uint8_t *buf)
{
    if (!read_at(a->f, c->offset, buf, c->size)) {
        ft_set_error("short read in the AVI");
        return false;
    }
    return true;
}

uint32_t ft_avi_max_chunk(const ft_avi *a)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < a->video_count; i++)
        m = a->video[i].size > m ? a->video[i].size : m;
    for (uint32_t i = 0; i < a->audio_count; i++)
        m = a->audio_chunks[i].size > m ? a->audio_chunks[i].size : m;
    return m;
}
