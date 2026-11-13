/*
 * ftconv — inspect, extract and decode the game's resource archives.
 *
 *   ftconv list       <game dir> [archive]                    list archives, or one archive's entries
 *   ftconv extract    <game dir> <archive> [entry] [-o dir]   raw entry bytes
 *   ftconv decode     <game dir> <archive> <entry> [-o path] [-f frame | --all-frames]
 *   ftconv decode-all <game dir> <out dir> [archive...]       every entry, into <out dir>/<archive>/
 *   ftconv verify     <game dir> [archive...]                 decode every entry, report problems
 *   ftconv script     <game dir> [script]                     parse all scripts (or dump one)
 *   ftconv run        <game dir> <script> [--choices 1,2,..] [--random seed] [--steps N] [-q]
 *                     play a script headless, printing dialogue and menu picks; after the
 *                     --choices list runs out, picks are random (--random) or the first entry
 *   ftconv i18n-template <game dir> [-o out.txt] [--from existing.txt] [--bare]
 *                     every translatable string as "key: " with the Japanese as a comment;
 *                     --from carries over the translations of an existing table; --bare leaves
 *                     out the Japanese (for tables kept in the repo)
 *
 * Decoded output: pseudo-BMP -> .png; ZDF -> .png (one frame) or a folder of NNN.png
 * (animation); script .txt -> UTF-8 .txt; .wav -> .wav as stored.
 */

#include "codec/avi.h"
#include "codec/bmp.h"
#include "codec/cinepak.h"
#include "codec/msadpcm.h"
#include "codec/text.h"
#include "codec/wav.h"
#include "codec/zdf.h"
#include "core/ft_common.h"
#include "game/i18n.h"
#include "script/interp.h"
#include "script/script.h"
#include "tools/png_write.h"
#include "vfs/archive.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define ft_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define ft_mkdir(p) mkdir(p, 0755)
#endif

static int usage(void)
{
    fputs("usage:\n"
          "  ftconv list       <game dir> [archive]\n"
          "  ftconv extract    <game dir> <archive> [entry] [-o dir]\n"
          "  ftconv decode     <game dir> <archive> <entry> [-o path] [-f frame | --all-frames]\n"
          "  ftconv decode-all <game dir> <out dir> [archive...]\n"
          "  ftconv verify     <game dir> [archive...]\n"
          "  ftconv script     <game dir> [script]\n"
          "  ftconv run        <game dir> <script> [--choices 1,2,..] [--random seed] [--steps N] [-q]\n"
          "  ftconv i18n-template <game dir> [-o out.txt] [--from existing.txt] [--bare]\n"
          "  ftconv movie      <game dir | file.avi> [-o base] [-f frame | --raw]\n",
          stderr);
    return 2;
}

static bool takes_value(const char *flag)
{
    return strcmp(flag, "-o") == 0 || strcmp(flag, "-f") == 0 || strcmp(flag, "--choices") == 0 ||
           strcmp(flag, "--steps") == 0 || strcmp(flag, "--random") == 0 || strcmp(flag, "--from") == 0;
}

static const char *opt_value(int argc, char **argv, const char *flag)
{
    for (int i = 0; i + 1 < argc; i++)
        if (strcmp(argv[i], flag) == 0)
            return argv[i + 1];
    return NULL;
}

static bool has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], flag) == 0)
            return true;
    return false;
}

/* Positional args, skipping flags (and the values of -o / -f). */
static int positional(int argc, char **argv, char **out, int max)
{
    int n = 0;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            if (takes_value(argv[i]))
                i++;
            continue;
        }
        if (n < max)
            out[n] = argv[i];
        n++;
    }
    return n;
}

static bool open_or_report(ft_archive *a, const char *dir, const char *name)
{
    if (ft_archive_open(a, dir, name))
        return true;
    fprintf(stderr, "error: %s\n", ft_get_error());
    return false;
}

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n >= e && ft_stricmp(name + n - e, ext) == 0;
}

static bool safe_name(const char *name)
{
    return name[0] && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && !strpbrk(name, "/\\:");
}

/* ---- list / extract ---------------------------------------------------- */

static int cmd_list(int argc, char **argv)
{
    char *pos[2];
    int n = positional(argc, argv, pos, 2);
    if (n < 1)
        return usage();

    if (n == 1) {
        for (size_t i = 0; i < ft_archive_name_count; i++) {
            ft_archive a;
            if (!open_or_report(&a, pos[0], ft_archive_names[i]))
                continue;
            printf("%-9s %5u entries  %-8s %12llu bytes\n", a.name, a.count, a.variable_layout ? "variable" : "fixed",
                   (unsigned long long)a.payload_size);
            ft_archive_close(&a);
        }
        return 0;
    }

    ft_archive a;
    if (!open_or_report(&a, pos[0], pos[1]))
        return 1;
    for (uint32_t i = 0; i < a.count; i++)
        printf("%10u %10u  %s\n", a.entries[i].offset, a.entries[i].size, a.entries[i].name);
    ft_archive_close(&a);
    return 0;
}

static bool extract_one(const ft_archive *a, const ft_entry *e, const char *out_dir)
{
    if (!safe_name(e->name)) {
        fprintf(stderr, "skipping unsafe entry name %s\n", e->name);
        return false;
    }
    uint8_t *data = ft_archive_read(a, e);
    if (!data) {
        fprintf(stderr, "error: %s\n", ft_get_error());
        return false;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", out_dir, e->name);
    bool ok = ft_write_file(path, data, e->size);
    if (!ok)
        fprintf(stderr, "error: %s\n", ft_get_error());
    free(data);
    return ok;
}

static int cmd_extract(int argc, char **argv)
{
    char *pos[3];
    int n = positional(argc, argv, pos, 3);
    if (n < 2)
        return usage();
    const char *out_dir = opt_value(argc, argv, "-o");
    if (!out_dir)
        out_dir = pos[1];
    ft_mkdir(out_dir);

    ft_archive a;
    if (!open_or_report(&a, pos[0], pos[1]))
        return 1;
    int failures = 0;
    if (n >= 3) {
        const ft_entry *e = ft_archive_find(&a, pos[2]);
        if (!e) {
            fprintf(stderr, "error: no entry %s in %s\n", pos[2], a.name);
            failures++;
        } else if (!extract_one(&a, e, out_dir)) {
            failures++;
        }
    } else {
        for (uint32_t i = 0; i < a.count; i++)
            failures += !extract_one(&a, &a.entries[i], out_dir);
        printf("%s: wrote %u entries to %s/\n", a.name, a.count - failures, out_dir);
    }
    ft_archive_close(&a);
    return failures ? 1 : 0;
}

/* ---- decoding to files ------------------------------------------------- */

typedef struct out_stats {
    uint32_t files;
    bool verbose;
} out_stats;

static bool write_png(out_stats *st, const char *path, const ft_image *img)
{
    if (!ft_png_write(path, img)) {
        fprintf(stderr, "error: %s\n", ft_get_error());
        return false;
    }
    st->files++;
    if (st->verbose)
        printf("wrote %s (%ux%u)\n", path, img->w, img->h);
    return true;
}

static bool write_raw(out_stats *st, const char *path, const void *data, size_t len)
{
    if (!ft_write_file(path, data, len)) {
        fprintf(stderr, "error: %s\n", ft_get_error());
        return false;
    }
    st->files++;
    if (st->verbose)
        printf("wrote %s\n", path);
    return true;
}

/*
 * ZDF -> <base>.png for a single-frame file or a single requested frame (frame >= 0),
 * otherwise a folder <base>/ with NNN.png per frame.
 */
static bool decode_zdf(out_stats *st, const uint8_t *data, size_t size, const char *base, int frame)
{
    ft_zdf z;
    if (!ft_zdf_open(&z, data, size)) {
        fprintf(stderr, "error: %s: %s\n", base, ft_get_error());
        return false;
    }
    if (st->verbose)
        printf("%s %ux%u, %u frame(s)\n", z.dfs0 ? "DFS0" : "DFS1", z.w, z.h, z.frame_count);
    if (frame >= 0 && (uint32_t)frame >= z.frame_count) {
        fprintf(stderr, "error: frame %d out of range\n", frame);
        ft_zdf_close(&z);
        return false;
    }
    bool folder = frame < 0 && z.frame_count > 1;
    if (folder)
        ft_mkdir(base);
    bool ok = true;
    for (uint32_t f = 0; f < z.frame_count && ok; f++) {
        if (!ft_zdf_next(&z, NULL)) {
            fprintf(stderr, "error: %s: %s\n", base, ft_get_error());
            ok = false;
            break;
        }
        if (frame >= 0 && f != (uint32_t)frame)
            continue;
        ft_image img;
        if (!ft_zdf_to_image(&z, &img)) {
            ok = false;
            break;
        }
        char path[1100];
        if (folder)
            snprintf(path, sizeof path, "%s/%03u.png", base, f);
        else
            snprintf(path, sizeof path, "%s.png", base);
        ok = write_png(st, path, &img);
        ft_image_free(&img);
        if (!folder)
            break;
    }
    ft_zdf_close(&z);
    return ok;
}

/* Decodes one entry to files named from `base` (no extension). frame: -1 = all frames. */
static bool decode_to_files(out_stats *st, uint8_t *data, size_t size, const char *name, const char *base, int frame)
{
    char path[1100];
    if (ft_bmp_is(data, size)) {
        ft_image img;
        ft_bmp_info info;
        if (!ft_bmp_decode(data, size, &img, &info)) {
            fprintf(stderr, "error: %s: %s\n", name, ft_get_error());
            return false;
        }
        if (st->verbose)
            printf("pseudo-BMP %ux%u %s, %u of %zu bytes used\n", img.w, img.h, info.has_alpha ? "alpha" : "opaque",
                   info.bytes_used, size);
        snprintf(path, sizeof path, "%s.png", base);
        bool ok = write_png(st, path, &img);
        ft_image_free(&img);
        return ok;
    }
    if (ft_zdf_is(data, size))
        return decode_zdf(st, data, size, base, frame);
    if (has_ext(name, ".txt")) {
        size_t len, bad;
        char *text = ft_text_decode(data, size, &len, &bad);
        if (!text) {
            fprintf(stderr, "error: %s: %s\n", name, ft_get_error());
            return false;
        }
        if (bad)
            fprintf(stderr, "warning: %s: %zu undecodable CP932 sequences\n", name, bad);
        snprintf(path, sizeof path, "%s.txt", base);
        bool ok = write_raw(st, path, text, len);
        free(text);
        return ok;
    }
    if (size >= 4 && memcmp(data, "RIFF", 4) == 0) {
        ft_wav wav;
        if (!ft_wav_parse(data, size, &wav)) {
            fprintf(stderr, "error: %s: %s\n", name, ft_get_error());
            return false;
        }
        if (st->verbose)
            printf("WAVE format %u, %u ch, %u Hz, %u bit, %u data bytes\n", wav.format, wav.channels, wav.sample_rate,
                   wav.bits_per_sample, wav.data_size);
        snprintf(path, sizeof path, "%s.wav", base);
        return write_raw(st, path, data, size);
    }
    fprintf(stderr, "error: %s: unknown format\n", name);
    return false;
}

/* Copies `name` without its extension into `stem`. */
static void stem_of(const char *name, char *stem, size_t cap)
{
    snprintf(stem, cap, "%s", name);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem)
        *dot = 0;
}

static int cmd_decode(int argc, char **argv)
{
    char *pos[3];
    if (positional(argc, argv, pos, 3) < 3)
        return usage();
    ft_archive a;
    if (!open_or_report(&a, pos[0], pos[1]))
        return 1;
    const ft_entry *e = ft_archive_find(&a, pos[2]);
    if (!e) {
        fprintf(stderr, "error: no entry %s in %s\n", pos[2], a.name);
        ft_archive_close(&a);
        return 1;
    }
    uint8_t *data = ft_archive_read(&a, e);
    if (!data) {
        fprintf(stderr, "error: %s\n", ft_get_error());
        ft_archive_close(&a);
        return 1;
    }

    /* Output base: -o minus a known extension, or the entry name minus its extension. */
    char base[1024];
    const char *out = opt_value(argc, argv, "-o");
    if (out) {
        snprintf(base, sizeof base, "%s", out);
        if (has_ext(base, ".png") || has_ext(base, ".txt") || has_ext(base, ".wav"))
            base[strlen(base) - 4] = 0;
    } else {
        stem_of(e->name, base, sizeof base);
    }
    const char *f = opt_value(argc, argv, "-f");
    int frame = has_flag(argc, argv, "--all-frames") ? -1 : f ? atoi(f) : 0;

    out_stats st = { 0, true };
    bool ok = decode_to_files(&st, data, e->size, e->name, base, frame);
    free(data);
    ft_archive_close(&a); /* e points into the archive's entry table */
    return ok ? 0 : 1;
}

static int decode_archive(const char *dir, const char *out_dir, const char *name, uint32_t *files_total)
{
    ft_archive a;
    if (!open_or_report(&a, dir, name))
        return 1;
    char sub[1024];
    snprintf(sub, sizeof sub, "%s/%s", out_dir, a.name);
    ft_mkdir(sub);

    clock_t t0 = clock();
    out_stats st = { 0, false };
    int failures = 0;
    for (uint32_t i = 0; i < a.count; i++) {
        const ft_entry *e = &a.entries[i];
        if (!safe_name(e->name)) {
            fprintf(stderr, "skipping unsafe entry name %s\n", e->name);
            failures++;
            continue;
        }
        /* Keep the extension in the name if another entry has the same stem. */
        char stem[FT_ENTRY_NAME_MAX], other[FT_ENTRY_NAME_MAX];
        stem_of(e->name, stem, sizeof stem);
        bool clash = false;
        for (uint32_t j = 0; j < a.count && !clash; j++) {
            if (j == i)
                continue;
            stem_of(a.entries[j].name, other, sizeof other);
            clash = ft_stricmp(stem, other) == 0;
        }
        char base[1200];
        const char *dot = strrchr(e->name, '.');
        if (clash)
            snprintf(base, sizeof base, "%s/%s_%s", sub, stem, dot ? dot + 1 : "entry");
        else
            snprintf(base, sizeof base, "%s/%s", sub, stem);

        uint8_t *data = ft_archive_read(&a, e);
        if (!data) {
            fprintf(stderr, "error: %s\n", ft_get_error());
            failures++;
            continue;
        }
        failures += !decode_to_files(&st, data, e->size, e->name, base, -1);
        free(data);
    }
    printf("%-9s %5u entries -> %6u files  %5.1fs%s\n", a.name, a.count, st.files,
           (double)(clock() - t0) / CLOCKS_PER_SEC, failures ? "  (FAILURES)" : "");
    fflush(stdout);
    *files_total += st.files;
    ft_archive_close(&a);
    return failures;
}

static int cmd_decode_all(int argc, char **argv)
{
    char *pos[32];
    int n = positional(argc, argv, pos, 32);
    if (n < 2 || n > 32)
        return usage();
    ft_mkdir(pos[1]);
    int failures = 0;
    uint32_t files = 0;
    if (n == 2) {
        for (size_t i = 0; i < ft_archive_name_count; i++)
            failures += decode_archive(pos[0], pos[1], ft_archive_names[i], &files);
    } else {
        for (int i = 2; i < n; i++)
            failures += decode_archive(pos[0], pos[1], pos[i], &files);
    }
    printf("%u files written to %s/, %d failure(s)\n", files, pos[1], failures);
    return failures ? 1 : 0;
}

/* ---- verify ------------------------------------------------------------ */

typedef struct verify_stats {
    uint32_t bmp, bmp_alpha, zdf, zdf_frames, dfs0, wav, txt;
    uint32_t bmp_header_mismatch, bmp_odd_alpha, zdf_odd_width, zdf_slack, wav_non_pcm;
    uint32_t problems;
} verify_stats;

static void problem(verify_stats *s, const ft_archive *a, const ft_entry *e, const char *what)
{
    if (s->problems < 40)
        printf("  PROBLEM %s/%s: %s\n", a->name, e->name, what);
    s->problems++;
}

static void verify_entry(verify_stats *s, const ft_archive *a, const ft_entry *e, uint8_t *data)
{
    char msg[600];
    if (ft_bmp_is(data, e->size)) {
        ft_image img;
        ft_bmp_info info;
        s->bmp++;
        if (!ft_bmp_decode(data, e->size, &img, &info)) {
            problem(s, a, e, ft_get_error());
            return;
        }
        s->bmp_alpha += info.has_alpha;
        if (info.bytes_used != e->size) {
            snprintf(msg, sizeof msg, "used %u of %u bytes", info.bytes_used, e->size);
            problem(s, a, e, msg);
        }
        if (info.bmp_width != (int32_t)img.w || (info.bmp_height != (int32_t)img.h && info.bmp_height != -(int32_t)img.h))
            s->bmp_header_mismatch++;
        s->bmp_odd_alpha += info.odd_alpha_tiles != 0;
        ft_image_free(&img);
    } else if (ft_zdf_is(data, e->size)) {
        ft_zdf z;
        s->zdf++;
        if (!ft_zdf_open(&z, data, e->size)) {
            problem(s, a, e, ft_get_error());
            return;
        }
        s->dfs0 += z.dfs0;
        s->zdf_odd_width += (z.w % 4) != 0;
        for (uint32_t f = 0; f < z.frame_count; f++) {
            if (!ft_zdf_next(&z, NULL)) {
                problem(s, a, e, ft_get_error());
                ft_zdf_close(&z);
                return;
            }
        }
        s->zdf_frames += z.frame_count;
        if (z.pos != z.len) {
            snprintf(msg, sizeof msg, "%u frames end at %zu of %zu bytes", z.frame_count, z.pos, z.len);
            problem(s, a, e, msg);
        }
        s->zdf_slack += z.delta_slack != 0;
        ft_zdf_close(&z);
    } else if (e->size >= 4 && memcmp(data, "RIFF", 4) == 0) {
        ft_wav wav;
        s->wav++;
        if (!ft_wav_parse(data, e->size, &wav)) {
            problem(s, a, e, ft_get_error());
            return;
        }
        s->wav_non_pcm += wav.format != 1;
    } else if (has_ext(e->name, ".txt")) {
        size_t len, bad;
        s->txt++;
        char *text = ft_text_decode(data, e->size, &len, &bad);
        if (!text) {
            problem(s, a, e, ft_get_error());
            return;
        }
        if (bad) {
            snprintf(msg, sizeof msg, "%zu undecodable CP932 sequences", bad);
            problem(s, a, e, msg);
        }
        free(text);
    } else {
        problem(s, a, e, "unknown format");
    }
}

static void verify_archive(verify_stats *s, const char *dir, const char *name)
{
    ft_archive a;
    if (!open_or_report(&a, dir, name)) {
        s->problems++;
        return;
    }
    uint32_t before = s->problems;
    if (a.max_end != a.payload_size) {
        printf("  PROBLEM %s: max(offset+size) %llu != payload size %llu\n", a.name, (unsigned long long)a.max_end,
               (unsigned long long)a.payload_size);
        s->problems++;
    }
    if (a.nonzero_flags) {
        printf("  PROBLEM %s: %u fixed records with a nonzero flag byte\n", a.name, a.nonzero_flags);
        s->problems++;
    }
    for (uint32_t i = 0; i < a.count; i++)
        for (uint32_t j = 0; j < i; j++)
            if (ft_stricmp(a.entries[i].name, a.entries[j].name) == 0) {
                printf("  PROBLEM %s: duplicate name %s\n", a.name, a.entries[i].name);
                s->problems++;
            }
    for (uint32_t i = 0; i < a.count; i++) {
        uint8_t *data = ft_archive_read(&a, &a.entries[i]);
        if (!data) {
            problem(s, &a, &a.entries[i], ft_get_error());
            continue;
        }
        verify_entry(s, &a, &a.entries[i], data);
        free(data);
    }
    printf("%-9s %5u entries  %s\n", a.name, a.count, s->problems == before ? "ok" : "PROBLEMS");
    fflush(stdout);
    ft_archive_close(&a);
}

static int cmd_verify(int argc, char **argv)
{
    char *pos[32];
    int n = positional(argc, argv, pos, 32);
    if (n < 1 || n > 32)
        return usage();
    verify_stats s = { 0 };
    if (n == 1) {
        for (size_t i = 0; i < ft_archive_name_count; i++)
            verify_archive(&s, pos[0], ft_archive_names[i]);
    } else {
        for (int i = 1; i < n; i++)
            verify_archive(&s, pos[0], pos[i]);
    }
    printf("\npseudo-BMP %u (%u alpha) | ZDF %u (%u DFS0, %u frames) | WAV %u | text %u\n", s.bmp, s.bmp_alpha, s.zdf,
           s.dfs0, s.zdf_frames, s.wav, s.txt);
    printf("notes: %u BMP header size mismatches, %u BMPs with odd alpha nibbles, %u ZDFs with width %% 4 != 0,\n"
           "       %u ZDFs with unused delta bytes, %u non-PCM WAVs\n",
           s.bmp_header_mismatch, s.bmp_odd_alpha, s.zdf_odd_width, s.zdf_slack, s.wav_non_pcm);
    printf("%s: %u problem(s)\n", s.problems ? "FAIL" : "PASS", s.problems);
    return s.problems ? 1 : 0;
}

/* ---- script ------------------------------------------------------------ */

typedef struct script_set {
    ft_script *s;
    uint32_t count;
} script_set;

static bool load_scripts(const char *dir, script_set *set)
{
    static const char *const archives[] = { "text", "text3d" };
    set->s = NULL;
    set->count = 0;
    for (size_t k = 0; k < 2; k++) {
        ft_archive a;
        if (!open_or_report(&a, dir, archives[k]))
            return false;
        ft_script *grown = realloc(set->s, sizeof *set->s * (set->count + a.count));
        if (!grown) {
            ft_archive_close(&a);
            return false;
        }
        set->s = grown;
        for (uint32_t i = 0; i < a.count; i++) {
            uint8_t *raw = ft_archive_read(&a, &a.entries[i]);
            size_t len = 0;
            char *text = raw ? ft_text_decode(raw, a.entries[i].size, &len, NULL) : NULL;
            free(raw);
            char stem[FT_ENTRY_NAME_MAX];
            stem_of(a.entries[i].name, stem, sizeof stem);
            if (!text || !ft_script_parse(&set->s[set->count], stem, text, len)) {
                fprintf(stderr, "error: %s: %s\n", a.entries[i].name, ft_get_error());
                free(text);
                ft_archive_close(&a);
                return false;
            }
            free(text);
            set->count++;
        }
        ft_archive_close(&a);
    }
    return true;
}

static const ft_script *find_script(const script_set *set, const char *name)
{
    char stem[FT_ENTRY_NAME_MAX];
    stem_of(name, stem, sizeof stem);
    for (uint32_t i = 0; i < set->count; i++)
        if (ft_stricmp(set->s[i].name, stem) == 0)
            return &set->s[i];
    return NULL;
}

static void dump_script(const ft_script *s)
{
    for (uint32_t i = 0; i < s->count; i++) {
        const ft_sline *l = &s->lines[i];
        if (l->kind == FT_LINE_BLANK)
            continue;
        printf("%5u %*s%-8s %s", l->line_no, l->indent * 2, "", ft_line_kind_name((ft_line_kind)l->kind), l->head);
        for (int a = 0; a < l->argc; a++)
            printf(" %s[%s]", a ? "" : "|", l->argv[a]);
        if (l->block_end >= 0)
            printf("  -> else line %u", l->block_end < (int32_t)s->count ? s->lines[l->block_end].line_no : 0);
        if (l->keyword == FT_KW_LOOP)
            printf("  <- back to line %u", l->loop_start >= 0 ? s->lines[l->loop_start].line_no : 0);
        putchar('\n');
    }
}

static int cmd_script(int argc, char **argv)
{
    char *pos[2];
    int n = positional(argc, argv, pos, 2);
    if (n < 1)
        return usage();
    script_set set;
    if (!load_scripts(pos[0], &set))
        return 1;

    int rc = 0;
    if (n >= 2) {
        const ft_script *s = find_script(&set, pos[1]);
        if (s)
            dump_script(s);
        else {
            fprintf(stderr, "error: no script %s\n", pos[1]);
            rc = 1;
        }
    } else {
        uint32_t kinds[8] = { 0 }, label_kinds[5] = { 0 }, lines = 0, jumps = 0, jump_hits[5] = { 0 };
        uint32_t unresolved = 0, bad_loads = 0, empty_if = 0, loop_no_body = 0;
        for (uint32_t i = 0; i < set.count; i++) {
            const ft_script *s = &set.s[i];
            for (uint32_t k = 0; k < s->count; k++) {
                const ft_sline *l = &s->lines[k];
                lines++;
                kinds[l->kind]++;
                if (l->kind == FT_LINE_LABEL)
                    label_kinds[l->label_kind]++;
                if (l->kind != FT_LINE_KEYWORD)
                    continue;
                if ((l->keyword == FT_KW_GOTO || l->keyword == FT_KW_CALL) && l->argc >= 1) {
                    jumps++;
                    int32_t t = ft_script_find_label(s, l->argv[0], FT_LABEL_NONE);
                    if (t < 0) {
                        if (unresolved++ < 25)
                            printf("  unresolved %s %s  (%s:%u)\n", l->head, l->argv[0], s->name, l->line_no);
                    } else {
                        jump_hits[s->lines[t].label_kind]++;
                    }
                }
                if ((l->keyword == FT_KW_LOAD || l->keyword == FT_KW_LOADJUMP) && l->argc >= 1 &&
                    !find_script(&set, l->argv[0])) {
                    if (bad_loads++ < 25)
                        printf("  missing script %s %s  (%s:%u)\n", l->head, l->argv[0], s->name, l->line_no);
                }
                if (l->keyword == FT_KW_IF && l->block_end == (int32_t)k + 1)
                    empty_if++;
                if (l->keyword == FT_KW_LOOP && l->loop_start < 0) {
                    if (loop_no_body++ < 10)
                        printf("  loop without an indented body  (%s:%u)\n", s->name, l->line_no);
                }
            }
        }
        printf("\n%u scripts, %u lines\n", set.count, lines);
        for (int k = 0; k < 8; k++)
            printf("  %-9s %6u\n", ft_line_kind_name((ft_line_kind)k), kinds[k]);
        printf("labels: %u ○, %u ＊, %u #, %u ☆\n", label_kinds[FT_LABEL_LOCATION], label_kinds[FT_LABEL_CHOICE],
               label_kinds[FT_LABEL_HASH], label_kinds[FT_LABEL_SAVE]);
        printf("goto/call: %u, resolved to ○ %u / ＊ %u / # %u / ☆ %u, unresolved %u\n", jumps,
               jump_hits[FT_LABEL_LOCATION], jump_hits[FT_LABEL_CHOICE], jump_hits[FT_LABEL_HASH],
               jump_hits[FT_LABEL_SAVE], unresolved);
        printf("load targets missing: %u | if with empty block: %u | loop without body: %u\n", bad_loads, empty_if,
               loop_no_body);
    }
    for (uint32_t i = 0; i < set.count; i++)
        ft_script_free(&set.s[i]);
    free(set.s);
    return rc;
}

/* ---- run (headless interpreter trace) ---------------------------------- */

typedef struct run_host {
    script_set *set;
    int picks[256];
    int pick_count, pick_next;
    bool quiet;
    bool random;
    uint32_t rng;
    uint32_t dialogue, commands, menus, scripts;
} run_host;

static const ft_script *run_load(void *user, const char *name)
{
    return find_script(((run_host *)user)->set, name);
}

static bool run_dialogue(void *user, ft_interp *in, const ft_sline *l)
{
    (void)in;
    run_host *h = user;
    h->dialogue++;
    if (!h->quiet)
        printf("  %s「%s」%s%s\n", l->head, l->argv[1], l->argv[0][0] ? " $" : "", l->argv[0]);
    return true;
}

static int run_choose(void *user, ft_interp *in, const ft_choice *c, int count)
{
    run_host *h = user;
    int pick = 0;
    if (h->pick_next < h->pick_count) {
        pick = h->picks[h->pick_next++] - 1;
    } else if (h->random) {
        /* skip the system menu verb: its save/load panel waits on UI input headless runs can't give */
        int usable = 0;
        for (int i = 0; i < count; i++)
            usable += strncmp(c[i].text, "system", 6) != 0;
        h->rng = h->rng * 1103515245u + 12345u;
        int n = usable ? (int)((h->rng >> 16) % (uint32_t)usable) : 0;
        for (int i = 0; i < count; i++)
            if (!usable || strncmp(c[i].text, "system", 6) != 0) {
                if (n-- == 0) {
                    pick = i;
                    break;
                }
            }
    }
    if (pick < 0 || pick >= count)
        pick = 0;
    h->menus++;
    if (!h->quiet) {
        printf("  [menu %s:%u]", in->script->name, in->script->lines[in->pc].line_no);
        for (int i = 0; i < count; i++)
            printf(" %s%s%s", i == pick ? ">" : "", c[i].text, i == pick ? "<" : "");
        putchar('\n');
    }
    return pick;
}

static bool run_command(void *user, ft_interp *in, const ft_sline *l)
{
    ((run_host *)user)->commands++;
    if (strcmp(l->head, "savepanel.select") == 0) /* leave the save panel at once (999 = back) */
        ft_vars_set_int(&in->vars, "number", 999);
    return false;
}

static void run_entered(void *user, ft_interp *in, const ft_script *s)
{
    (void)in;
    run_host *h = user;
    h->scripts++;
    if (!h->quiet)
        printf("== %s\n", s->name);
}

static int cmd_run(int argc, char **argv)
{
    char *pos[2];
    if (positional(argc, argv, pos, 2) < 2)
        return usage();
    script_set set;
    if (!load_scripts(pos[0], &set))
        return 1;

    run_host h = { &set, { 0 }, 0, 0, has_flag(argc, argv, "-q"), false, 0, 0, 0, 0, 0 };
    const char *seed = opt_value(argc, argv, "--random");
    if (seed) {
        h.random = true;
        h.rng = (uint32_t)strtoul(seed, NULL, 10);
    }
    const char *choices = opt_value(argc, argv, "--choices");
    for (const char *p = choices; p && *p && h.pick_count < 256;) {
        h.picks[h.pick_count++] = atoi(p);
        p = strchr(p, ',');
        p = p ? p + 1 : NULL;
    }
    const char *steps_opt = opt_value(argc, argv, "--steps");
    uint64_t max_steps = steps_opt ? strtoull(steps_opt, NULL, 10) : 500000;

    ft_host host = { &h, run_load, run_dialogue, run_choose, run_command, run_entered };
    ft_interp in;
    ft_interp_init(&in, &host);
    ft_run r = FT_RUN_ERROR;
    if (ft_interp_boot(&in, pos[1])) {
        do
            r = ft_interp_step(&in);
        while ((r == FT_RUN_CONTINUE || r == FT_RUN_WAIT) && in.steps < max_steps);
    }

    const char *how = r == FT_RUN_END ? "end" : r == FT_RUN_ERROR ? "ERROR" : "step limit";
    printf("\nstopped: %s", how);
    if (in.script)
        printf(" at %s:%u", in.script->name, in.pc < in.script->count ? in.script->lines[in.pc].line_no : 0);
    printf("\n%llu lines executed, %u scripts entered, %u dialogue lines, %u menus, %u other commands\n",
           (unsigned long long)in.steps, h.scripts, h.dialogue, h.menus, h.commands);
    printf("%u label warnings%s%s | %u variable errors%s%s | %u variables\n", in.warnings,
           in.warnings ? ", last: " : "", in.warnings ? in.error : "", in.vars.errors,
           in.vars.errors ? ", last: " : "", in.vars.errors ? in.vars.last_error : "", in.vars.count);
    if (r == FT_RUN_ERROR)
        printf("error: %s\n", in.error);

    ft_interp_free(&in);
    for (uint32_t i = 0; i < set.count; i++)
        ft_script_free(&set.s[i]);
    free(set.s);
    return r == FT_RUN_ERROR ? 1 : 0;
}

/* ---- i18n-template ------------------------------------------------------ */

typedef struct key_set {
    char **k;
    uint32_t count, cap;
} key_set;

/* Adds `key`; false if it was already there (linear is fine for a few thousand keys once). */
static bool key_add(key_set *s, const char *key)
{
    for (uint32_t i = 0; i < s->count; i++)
        if (strcmp(s->k[i], key) == 0)
            return false;
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 1024;
        char **grown = realloc(s->k, sizeof *s->k * s->cap);
        if (!grown)
            return false;
        s->k = grown;
    }
    s->k[s->count] = malloc(strlen(key) + 1);
    if (!s->k[s->count])
        return false;
    strcpy(s->k[s->count++], key);
    return true;
}

static void unquote_arg(char *dst, size_t cap, const char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"')
        snprintf(dst, cap, "%.*s", (int)(n - 2), s + 1);
    else
        snprintf(dst, cap, "%s", s);
}

static int cmp_script_name(const void *a, const void *b)
{
    return ft_stricmp((*(const ft_script *const *)a)->name, (*(const ft_script *const *)b)->name);
}

/* The base verb of a %cmnd head: "%cmnd.look.call" -> "look". */
static void cmnd_verb(const char *head, char *out, size_t cap)
{
    snprintf(out, cap, "%s", head + 6);
    char *dot = strchr(out, '.');
    if (dot)
        *dot = 0;
}

/* Writes "# where  original" (just "# where" with --bare) then "key: translation" once per key. */
static bool g_bare;

static void emit(FILE *f, key_set *seen, const ft_i18n *prev, const char *key, const char *where, const char *original,
                 uint32_t *count, uint32_t *done)
{
    if (!key_add(seen, key))
        return;
    const char *have = ft_i18n_get(prev, key);
    fprintf(f, "# %s%s%s\n%s: ", where, g_bare ? "" : "  ", g_bare ? "" : original, key);
    if (have) {
        for (const char *p = have; *p; p++) /* back to one line */
            if (*p == '\n')
                fputs("\\n", f);
            else
                fputc(*p, f);
        (*done)++;
    }
    fputs("\n", f);
    (*count)++;
}

static int cmd_i18n_template(int argc, char **argv)
{
    char *pos[1];
    if (positional(argc, argv, pos, 1) < 1)
        return usage();
    const char *out = opt_value(argc, argv, "-o");
    const char *from = opt_value(argc, argv, "--from");
    g_bare = has_flag(argc, argv, "--bare");
    script_set set;
    if (!load_scripts(pos[0], &set))
        return 1;
    ft_i18n prev = { 0 };
    if (from && !ft_i18n_load(&prev, from)) {
        fprintf(stderr, "error: %s: %s\n", from, ft_get_error());
        return 1;
    }
    FILE *f = out ? fopen(out, "wb") : stdout;
    if (!f) {
        fprintf(stderr, "error: can't write %s\n", out);
        return 1;
    }
    fputs("# fantatech translation table (see src/game/i18n.h). One \"key: text\" per line; empty = not\n"
          "# translated. Dialogue keys are the script's $id, other keys hash the Japanese (FNV-1a).\n"
          "# \\n breaks a line. Optional: @font: <path to .ttf/.ttc> for translated text, @name: <language>.\n",
          f);
    if (prev.font[0])
        fprintf(f, "@font: %s\n", prev.font);
    if (prev.name[0])
        fprintf(f, "@name: %s\n", prev.name);
    key_set seen = { 0 };
    uint32_t count = 0, done = 0;
    char key[64], where[96], text[512], verb[32];
    /* our own UI text: not the game's, so the Japanese stays in even with --bare */
    fputs("\n## engine\n", f);
    bool bare = g_bare;
    g_bare = false;
    for (size_t i = 0; i < ft_ui_string_count; i++) {
        ft_i18n_key(ft_ui_strings[i], key);
        emit(f, &seen, &prev, key, "engine", ft_ui_strings[i], &count, &done);
    }
    g_bare = bare;
    const ft_script **order = malloc(sizeof *order * (set.count ? set.count : 1)); /* by name */
    if (!order)
        return 1;
    for (uint32_t i = 0; i < set.count; i++)
        order[i] = &set.s[i];
    qsort(order, set.count, sizeof *order, cmp_script_name);
    for (uint32_t i = 0; i < set.count; i++) {
        const ft_script *s = order[i];
        bool header = false;
        for (uint32_t k = 0; k < s->count; k++) {
            const ft_sline *l = &s->lines[k];
            snprintf(where, sizeof where, "%s:%u", s->name, l->line_no);
            const char *strings[64];
            char buf[64][256];
            int n = 0;
            if (l->kind == FT_LINE_DIALOGUE && l->argc >= 2) {
                if (!header)
                    fprintf(f, "\n## %s\n", s->name), header = true;
                if (l->argv[0][0])
                    snprintf(key, sizeof key, "%s", l->argv[0]);
                else
                    ft_i18n_key(l->argv[1], key);
                emit(f, &seen, &prev, key, where, l->text, &count, &done);
                continue;
            }
            if (l->kind != FT_LINE_COMMAND)
                continue;
            if (strcmp(l->head, "%select.set") == 0 && l->argc > 0) {
                /* %select.set label[,text]: the label is shown when there's no text */
                unquote_arg(buf[n], sizeof buf[0], l->argv[l->argc > 1 ? 1 : 0]);
                strings[n] = buf[n], n++;
            } else if ((strcmp(l->head, "select") == 0 || strcmp(l->head, "select02") == 0 ||
                        strcmp(l->head, "selectCancel") == 0)) {
                for (int a = 0; a < l->argc && n < 64; a++) {
                    unquote_arg(buf[n], sizeof buf[0], l->argv[a]);
                    strings[n] = buf[n], n++;
                }
            } else if (strncmp(l->head, "%cmnd.", 6) == 0 && strcmp(l->head, "%cmnd.wait") != 0 &&
                       strncmp(l->head, "%cmnd.new", 9) != 0 && strcmp(l->head, "%cmnd.clear") != 0 &&
                       strcmp(l->head, "%cmnd.delete") != 0 && l->argc > 0) {
                cmnd_verb(l->head, verb, sizeof verb); /* the runtime shows "verb: label" */
                snprintf(buf[n], sizeof buf[0], "%s", verb);
                strings[n] = buf[n], n++;
                snprintf(buf[n], sizeof buf[0], "%s", l->argv[0]);
                strings[n] = buf[n], n++;
            }
            for (int a = 0; a < n; a++) {
                if (!strings[a][0])
                    continue;
                if (!header)
                    fprintf(f, "\n## %s\n", s->name), header = true;
                ft_i18n_key(strings[a], key);
                snprintf(text, sizeof text, "%s", strings[a]);
                emit(f, &seen, &prev, key, where, text, &count, &done);
            }
        }
    }
    free(order);
    if (out)
        fclose(f);
    fprintf(stderr, "%u strings, %u translated\n", count, done);
    for (uint32_t i = 0; i < seen.count; i++)
        free(seen.k[i]);
    free(seen.k);
    ft_i18n_free(&prev);
    for (uint32_t i = 0; i < set.count; i++)
        ft_script_free(&set.s[i]);
    free(set.s);
    return 0;
}

/* ---- movie ------------------------------------------------------------- */

static bool write_wav16(const char *path, const int16_t *pcm, uint32_t frames, uint16_t ch, uint32_t rate)
{
    uint32_t data = frames * ch * 2;
    uint8_t h[44] = "RIFF....WAVEfmt ";
    uint32_t v[] = { 36 + data };
    memcpy(h + 4, v, 4);
    const uint8_t fmt[] = { 16, 0, 0, 0, 1, 0, (uint8_t)ch, 0 };
    memcpy(h + 16, fmt, 8);
    uint32_t r[] = { rate, rate * ch * 2 };
    memcpy(h + 24, r, 8);
    const uint8_t align[] = { (uint8_t)(ch * 2), 0, 16, 0, 'd', 'a', 't', 'a' };
    memcpy(h + 32, align, 8);
    memcpy(h + 40, &data, 4); /* little-endian hosts only, like the rest of ftconv's writers */
    FILE *f = ft_fopen(path, "wb");
    bool ok = f && fwrite(h, 1, 44, f) == 44 && fwrite(pcm, 2, (size_t)frames * ch, f) == (size_t)frames * ch;
    if (f)
        ok = fclose(f) == 0 && ok;
    if (!ok)
        fprintf(stderr, "error: can't write %s\n", path);
    return ok;
}

/*
 * The intro movie: <base>.wav (all the audio) and <base>.png (frame -f, default 0), or with --raw
 * <base>.rgb, every frame as RGB24 back to back, to compare with
 *   ffmpeg -i movie0508_cinepak.avi -f rawvideo -pix_fmt rgb24 ref.rgb
 *   ffmpeg -i movie0508_cinepak.avi -f s16le ref.pcm
 */
static int cmd_movie(int argc, char **argv)
{
    char *pos[1];
    if (positional(argc, argv, pos, 1) < 1)
        return usage();
    char path[1100];
    size_t len = strlen(pos[0]);
    if (len > 4 && ft_stricmp(pos[0] + len - 4, ".avi") == 0)
        snprintf(path, sizeof path, "%s", pos[0]);
    else
        snprintf(path, sizeof path, "%s/movie0508_cinepak.avi", pos[0]);
    const char *out = opt_value(argc, argv, "-o");
    const char *f = opt_value(argc, argv, "-f");
    bool raw = has_flag(argc, argv, "--raw");
    uint32_t want = f ? (uint32_t)atoi(f) : 0;
    char base[1024];
    snprintf(base, sizeof base, "%s", out ? out : "movie");

    ft_avi a;
    if (!ft_avi_open(&a, path)) {
        fprintf(stderr, "error: %s\n", ft_get_error());
        return 1;
    }
    printf("%ux%u %.4s, %u frames at %u/%u fps; audio: format %u, %u ch, %u Hz, block %u, %u chunks\n", a.width,
           a.height, (const char *)&a.video_fourcc, a.video_count, a.rate, a.scale, a.audio.format, a.audio.channels,
           a.audio.sample_rate, a.audio.block_align, a.audio_count);
    uint8_t *buf = malloc(ft_avi_max_chunk(&a) + 1);
    ft_cinepak *cv = malloc(sizeof *cv);
    int rc = 1;
    if (!buf || !cv || !ft_cinepak_init(cv, a.width, a.height))
        goto done;

    /* audio: every chunk, block by block */
    if (a.has_audio && a.audio.format == 2) {
        size_t frames = 0;
        for (uint32_t i = 0; i < a.audio_count; i++)
            for (uint32_t off = 0; off < a.audio_chunks[i].size; off += a.audio.block_align)
                frames += ft_msadpcm_block_samples(&a.audio, a.audio_chunks[i].size - off);
        int16_t *pcm = malloc(frames * a.audio.channels * sizeof *pcm + 1);
        uint32_t got = 0;
        for (uint32_t i = 0; pcm && i < a.audio_count; i++) {
            if (!ft_avi_read(&a, &a.audio_chunks[i], buf))
                break;
            got += ft_msadpcm_decode(&a.audio, buf, a.audio_chunks[i].size, pcm + (size_t)got * a.audio.channels);
        }
        char wav[1100];
        snprintf(wav, sizeof wav, "%s.wav", base);
        bool ok = pcm && write_wav16(wav, pcm, got, a.audio.channels, a.audio.sample_rate);
        free(pcm);
        if (!ok)
            goto done;
        printf("wrote %s (%u samples per channel, %.2f s)\n", wav, got, (double)got / a.audio.sample_rate);
    }

    /* video: in order, since frames build on the one before */
    FILE *rgb = NULL;
    if (raw) {
        char p[1100];
        snprintf(p, sizeof p, "%s.rgb", base);
        if (!(rgb = ft_fopen(p, "wb"))) {
            fprintf(stderr, "error: can't write %s\n", p);
            goto done;
        }
    }
    size_t frame_bytes = (size_t)a.width * a.height * 3;
    uint32_t last = raw ? a.video_count : (want < a.video_count ? want + 1 : 0);
    for (uint32_t i = 0; i < last; i++) {
        if (!ft_avi_read(&a, &a.video[i], buf))
            break;
        if (a.video[i].size && !ft_cinepak_decode(cv, buf, a.video[i].size))
            fprintf(stderr, "warning: frame %u: %s\n", i, ft_get_error());
        if (rgb && fwrite(cv->rgb, 1, frame_bytes, rgb) != frame_bytes)
            break;
    }
    if (rgb) {
        fclose(rgb);
        printf("wrote %s.rgb (%u frames)\n", base, a.video_count);
    } else if (last) {
        ft_image img;
        if (!ft_image_alloc(&img, a.width, a.height))
            goto done;
        for (size_t p = 0; p < (size_t)a.width * a.height; p++) {
            img.bgra[p * 4] = cv->rgb[p * 3 + 2];
            img.bgra[p * 4 + 1] = cv->rgb[p * 3 + 1];
            img.bgra[p * 4 + 2] = cv->rgb[p * 3];
            img.bgra[p * 4 + 3] = 255;
        }
        char p[1100];
        snprintf(p, sizeof p, "%s.png", base);
        out_stats st = { 0, true };
        bool ok = write_png(&st, p, &img);
        ft_image_free(&img);
        if (!ok)
            goto done;
    }
    rc = 0;
done:
    if (cv)
        ft_cinepak_free(cv);
    free(cv);
    free(buf);
    ft_avi_close(&a);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    const char *cmd = argv[1];
    if (strcmp(cmd, "list") == 0)
        return cmd_list(argc - 2, argv + 2);
    if (strcmp(cmd, "extract") == 0)
        return cmd_extract(argc - 2, argv + 2);
    if (strcmp(cmd, "decode") == 0)
        return cmd_decode(argc - 2, argv + 2);
    if (strcmp(cmd, "decode-all") == 0)
        return cmd_decode_all(argc - 2, argv + 2);
    if (strcmp(cmd, "verify") == 0)
        return cmd_verify(argc - 2, argv + 2);
    if (strcmp(cmd, "script") == 0)
        return cmd_script(argc - 2, argv + 2);
    if (strcmp(cmd, "run") == 0)
        return cmd_run(argc - 2, argv + 2);
    if (strcmp(cmd, "i18n-template") == 0)
        return cmd_i18n_template(argc - 2, argv + 2);
    if (strcmp(cmd, "movie") == 0)
        return cmd_movie(argc - 2, argv + 2);
    return usage();
}
