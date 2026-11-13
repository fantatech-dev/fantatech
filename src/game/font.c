#include "game/font.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <string.h>

static TTF_Font *g_font;

/* FT_FONT overrides; otherwise MS Gothic (face 0 of msgothic.ttc) from the Windows fonts,
   or the platform's stock Japanese gothic elsewhere. */
static TTF_Font *open_font(void)
{
    const char *env = SDL_getenv("FT_FONT");
    if (env && env[0])
        return TTF_OpenFont(env, 24.0f);
#ifdef _WIN32
    const char *windir = SDL_getenv("WINDIR");
    char path[512];
    SDL_snprintf(path, sizeof path, "%s\\Fonts\\msgothic.ttc", windir && windir[0] ? windir : "C:\\Windows");
    return TTF_OpenFont(path, 24.0f);
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
    for (size_t i = 0; i < SDL_arraysize(paths); i++) {
        TTF_Font *f = TTF_OpenFont(paths[i], 24.0f);
        if (f)
            return f;
    }
    SDL_SetError("no Japanese font found (MS Gothic, Hiragino, IPA or Noto CJK)");
    return NULL;
#endif
}

bool ft_font_open(void)
{
    if (g_font)
        return true;
    if (!TTF_Init()) {
        ft_set_error("SDL_ttf: %s", SDL_GetError());
        return false;
    }
    g_font = open_font();
    if (!g_font) {
        ft_set_error("font: %s (set FT_FONT to a Japanese .ttf/.ttc)", SDL_GetError());
        TTF_Quit();
        return false;
    }
    return true;
}

bool ft_font_open_file(const char *path)
{
    if (!ft_font_open())
        return false;
    TTF_Font *f = TTF_OpenFont(path, 24.0f);
    if (!f) {
        ft_set_error("font %s: %s", path, SDL_GetError());
        return false;
    }
    TTF_CloseFont(g_font);
    g_font = f;
    return true;
}

bool ft_font_reset(void)
{
    if (!g_font)
        return ft_font_open();
    TTF_Font *f = open_font();
    if (!f) {
        ft_set_error("font: %s", SDL_GetError());
        return false;
    }
    TTF_CloseFont(g_font);
    g_font = f;
    return true;
}

void ft_font_close(void)
{
    if (!g_font)
        return;
    TTF_CloseFont(g_font);
    g_font = NULL;
    TTF_Quit();
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
    TTF_SetFontSize(g_font, (float)pixel_height);
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
