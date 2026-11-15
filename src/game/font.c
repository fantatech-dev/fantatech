#include "game/font.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <string.h>

static TTF_Font *g_font;        /* what text is drawn with */
static TTF_Font *g_fallback;    /* the Japanese gothic behind a Latin g_font, or NULL */
static char g_path[512];        /* their files, for bug reports */
static char g_fallback_path[512];

static TTF_Font *open_path(const char *path, char *used)
{
    TTF_Font *f = TTF_OpenFont(path, 24.0f);
    if (f)
        SDL_strlcpy(used, path, sizeof g_path);
    return f;
}

/* The first of `paths` (relative to `dir` when given) that opens. */
static TTF_Font *open_first(const char *dir, const char *const *paths, size_t n, char *used)
{
    char path[512];
    for (size_t i = 0; i < n; i++) {
        if (dir)
            SDL_snprintf(path, sizeof path, "%s%s", dir, paths[i]);
        else
            SDL_strlcpy(path, paths[i], sizeof path);
        TTF_Font *f = open_path(path, used);
        if (f)
            return f;
    }
    return NULL;
}

/* FT_FONT, else MS Gothic (face 0 of msgothic.ttc) from the Windows fonts, or the platform's
   stock Japanese gothic elsewhere. */
static TTF_Font *open_gothic(char *used)
{
    const char *env = SDL_getenv("FT_FONT");
    if (env && env[0])
        return open_path(env, used);
#ifdef _WIN32
    const char *windir = SDL_getenv("WINDIR");
    char path[512];
    SDL_snprintf(path, sizeof path, "%s\\Fonts\\msgothic.ttc", windir && windir[0] ? windir : "C:\\Windows");
    return open_path(path, used);
#else
    static const char *const paths[] = {
        "/Library/Fonts/msgothic.ttc",
        "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/usr/share/fonts/truetype/msttcorefonts/msgothic.ttc",
        "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
        "/usr/share/fonts/OTF/ipag.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    };
    TTF_Font *f = open_first(NULL, paths, SDL_arraysize(paths), used);
    if (!f)
        SDL_SetError("no Japanese font found (MS Gothic, Hiragino, IPA or Noto CJK)");
    return f;
#endif
}

/* A proportional sans for Latin text: the platform's UI font where its path is fixed, else the
   common free ones. SDL_ttf doesn't use fontconfig, so these are known install paths. */
static TTF_Font *open_latin(char *used)
{
#ifdef _WIN32
    static const char *const paths[] = { "\\Fonts\\tahoma.ttf", "\\Fonts\\arial.ttf" }; /* Tahoma: XP's UI font */
    const char *windir = SDL_getenv("WINDIR");
    return open_first(windir && windir[0] ? windir : "C:\\Windows", paths, SDL_arraysize(paths), used);
#else
    static const char *const paths[] = {
        "/System/Library/Fonts/Helvetica.ttc",                         /* macOS */
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",             /* Debian, Ubuntu */
        "/usr/share/fonts/TTF/DejaVuSans.ttf",                         /* Arch */
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",           /* Fedora */
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
    };
    return open_first(NULL, paths, SDL_arraysize(paths), used);
#endif
}

static void close_fonts(void)
{
    if (g_font)
        TTF_ClearFallbackFonts(g_font);
    if (g_fallback)
        TTF_CloseFont(g_fallback);
    if (g_font)
        TTF_CloseFont(g_font);
    g_font = g_fallback = NULL;
    g_path[0] = g_fallback_path[0] = 0;
}

/* Replaces the fonts with `font` (from `path`) and, when given, the gothic behind it. */
static void use_fonts(TTF_Font *font, const char *path, TTF_Font *fallback, const char *fallback_path)
{
    close_fonts();
    if (fallback && !TTF_AddFallbackFont(font, fallback)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "font fallback: %s", SDL_GetError());
        TTF_CloseFont(fallback);
        fallback = NULL;
    }
    g_font = font;
    g_fallback = fallback;
    SDL_strlcpy(g_path, path, sizeof g_path);
    SDL_strlcpy(g_fallback_path, fallback ? fallback_path : "", sizeof g_fallback_path);
}

bool ft_font_open(void)
{
    if (g_font)
        return true;
    if (!TTF_Init()) {
        ft_set_error("SDL_ttf: %s", SDL_GetError());
        return false;
    }
    char used[sizeof g_path];
    TTF_Font *f = open_gothic(used);
    if (!f) {
        ft_set_error("font: %s (set FT_FONT to a Japanese .ttf/.ttc)", SDL_GetError());
        TTF_Quit();
        return false;
    }
    use_fonts(f, used, NULL, NULL);
    return true;
}

bool ft_font_open_file(const char *path)
{
    if (!ft_font_open())
        return false;
    char used[sizeof g_path], gothic_used[sizeof g_path];
    TTF_Font *f = open_path(path, used);
    if (!f) {
        ft_set_error("font %s: %s", path, SDL_GetError());
        return false;
    }
    TTF_Font *gothic = open_gothic(gothic_used); /* for Japanese the table's font lacks */
    use_fonts(f, used, gothic, gothic_used);
    return true;
}

bool ft_font_reset(bool latin)
{
    if (!g_font)
        return ft_font_open() && (!latin || ft_font_reset(true));
    char gothic_used[sizeof g_path], latin_used[sizeof g_path];
    TTF_Font *gothic = open_gothic(gothic_used);
    if (!gothic) {
        ft_set_error("font: %s", SDL_GetError());
        return false;
    }
    const char *env = SDL_getenv("FT_FONT");
    TTF_Font *lat = latin && !(env && env[0]) ? open_latin(latin_used) : NULL; /* FT_FONT is the only font */
    if (lat)
        use_fonts(lat, latin_used, gothic, gothic_used);
    else
        use_fonts(gothic, gothic_used, NULL, NULL);
    return true;
}

void ft_font_close(void)
{
    if (!g_font)
        return;
    close_fonts();
    TTF_Quit();
}

static void describe_one(TTF_Font *f, const char *path, char *out, size_t cap)
{
    const char *family = TTF_GetFontFamilyName(f), *style = TTF_GetFontStyleName(f);
    SDL_snprintf(out, cap, "%s %s, %s", family ? family : "?", style ? style : "", path);
}

void ft_font_describe(char *out, size_t cap)
{
    int v = TTF_Version();
    char main_font[640] = "none", fallback[640] = "";
    if (g_font)
        describe_one(g_font, g_path, main_font, sizeof main_font);
    if (g_fallback) {
        char f[600];
        describe_one(g_fallback, g_fallback_path, f, sizeof f);
        SDL_snprintf(fallback, sizeof fallback, "; fallback %s", f);
    }
    SDL_snprintf(out, cap, "%s%s (SDL_ttf %d.%d.%d)", main_font, fallback, SDL_VERSIONNUM_MAJOR(v),
                 SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
}

static bool render(const char *utf8, int pixel_height, int wrap_w, ft_image *out)
{
    out->bgra = NULL;
    out->w = out->h = 0;
    if (!g_font) {
        ft_set_error("no font loaded");
        return false;
    }
    if (!utf8[0]) {
        ft_set_error("empty text");
        return false;
    }
    /* SDL_ttf resizes only the font it's given; a fallback has to match it */
    TTF_SetFontSize(g_font, (float)pixel_height);
    if (g_fallback)
        TTF_SetFontSize(g_fallback, (float)pixel_height);
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *s;
    if (wrap_w > 0) {
        TTF_SetFontWrapAlignment(g_font, TTF_HORIZONTAL_ALIGN_CENTER);
        s = TTF_RenderText_Blended_Wrapped(g_font, utf8, 0, white, wrap_w);
    } else {
        s = TTF_RenderText_Blended(g_font, utf8, 0, white);
    }
    if (!s) {
        ft_set_error("text rasterization failed: %s", SDL_GetError());
        return false;
    }
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_BGRA32);
    SDL_DestroySurface(s);
    if (!c) {
        ft_set_error("text surface conversion failed: %s", SDL_GetError());
        return false;
    }
    bool ok = ft_image_alloc(out, (uint32_t)c->w, (uint32_t)c->h);
    if (ok)
        for (int y = 0; y < c->h; y++)
            memcpy(out->bgra + (size_t)y * (size_t)c->w * 4, (const uint8_t *)c->pixels + (size_t)y * (size_t)c->pitch,
                   (size_t)c->w * 4);
    SDL_DestroySurface(c);
    return ok;
}

bool ft_font_render(const char *utf8, int pixel_height, ft_image *out)
{
    return render(utf8, pixel_height, 0, out);
}

bool ft_font_render_wrapped(const char *utf8, int pixel_height, int wrap_w, ft_image *out)
{
    return render(utf8, pixel_height, wrap_w, out);
}
