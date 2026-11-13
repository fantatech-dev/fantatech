#include "game/ui.h"

#include "game/font.h"

#include <string.h>

/* ---- text -------------------------------------------------------------- */

void ft_tex_free(ft_tex *t)
{
    if (t->t)
        SDL_DestroyTexture(t->t);
    memset(t, 0, sizeof *t);
}

ft_tex ft_ui_render_text(ft_ui *u, const char *utf8, int px)
{
    ft_tex t = { 0 };
    ft_image img;
    if (!utf8[0] || !ft_font_render(utf8, px, &img))
        return t;
    t.t = SDL_CreateTexture(u->r, SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_STATIC, (int)img.w, (int)img.h);
    if (t.t) {
        SDL_UpdateTexture(t.t, NULL, img.bgra, (int)img.w * 4);
        SDL_SetTextureBlendMode(t.t, SDL_BLENDMODE_BLEND);
        t.w = (float)img.w;
        t.h = (float)img.h;
    }
    ft_image_free(&img);
    return t;
}

const char *ft_ui_tr(const ft_ui *u, const char *jp)
{
    const char *t = ft_i18n_text(u->i18n, jp);
    return t ? t : jp;
}

const ft_tex *ft_ui_text(ft_ui *u, const char *jp, int px)
{
    for (int i = 0; i < u->cached; i++)
        if (u->cache[i].s == jp && u->cache[i].px == px)
            return &u->cache[i].t;
    if (u->cached == FT_UI_CACHE)
        return NULL;
    u->cache[u->cached].s = jp;
    u->cache[u->cached].px = px;
    u->cache[u->cached].t = ft_ui_render_text(u, ft_ui_tr(u, jp), px);
    return &u->cache[u->cached++].t;
}

void ft_ui_clear(ft_ui *u)
{
    for (int i = 0; i < u->cached; i++)
        ft_tex_free(&u->cache[i].t);
    u->cached = 0;
}

void ft_ui_draw_tex(ft_ui *u, const ft_tex *t, float x, float cy, SDL_Color c, bool centre, float max_w)
{
    if (!t || !t->t)
        return;
    float w = max_w > 0 && t->w > max_w ? max_w : t->w;
    SDL_FRect src = { 0, 0, w, t->h };
    SDL_FRect dst = { SDL_floorf(centre ? x - w / 2 : x), SDL_floorf(cy - t->h / 2), w, t->h };
    SDL_SetTextureColorMod(t->t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t->t, c.a);
    SDL_RenderTexture(u->r, t->t, &src, &dst);
}

void ft_ui_draw_text(ft_ui *u, const char *jp, int px, float x, float cy, SDL_Color c, bool centre)
{
    ft_ui_draw_tex(u, ft_ui_text(u, jp, px), x, cy, c, centre, 0);
}

void ft_ui_draw_label(ft_ui *u, const char *utf8, int px, float x, float cy, SDL_Color c, bool centre)
{
    ft_tex t = ft_ui_render_text(u, utf8, px);
    ft_ui_draw_tex(u, &t, x, cy, c, centre, 0);
    ft_tex_free(&t);
}

/* ---- shapes ------------------------------------------------------------ */

SDL_Color ft_ui_alpha(SDL_Color c, float a)
{
    c.a = (Uint8)((float)c.a * a);
    return c;
}

void ft_ui_fill(ft_ui *u, SDL_FRect r, SDL_Color c)
{
    SDL_SetRenderDrawColor(u->r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(u->r, &r);
}

void ft_ui_outline(ft_ui *u, SDL_FRect r, SDL_Color c)
{
    SDL_SetRenderDrawColor(u->r, c.r, c.g, c.b, c.a);
    SDL_RenderRect(u->r, &r);
}

void ft_ui_circle(ft_ui *u, float cx, float cy, float rad, bool filled, SDL_Color c)
{
    enum { N = 32 };
    SDL_FPoint pt[N + 1];
    for (int i = 0; i <= N; i++) {
        float a = (float)i * 2.0f * SDL_PI_F / N;
        pt[i] = (SDL_FPoint){ cx + rad * SDL_cosf(a), cy + rad * SDL_sinf(a) };
    }
    if (!filled) {
        SDL_SetRenderDrawColor(u->r, c.r, c.g, c.b, c.a);
        SDL_RenderLines(u->r, pt, N + 1);
        return;
    }
    SDL_FColor fc = { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f };
    SDL_Vertex v[N + 1];
    int idx[N * 3];
    v[0] = (SDL_Vertex){ { cx, cy }, fc, { 0, 0 } };
    for (int i = 0; i < N; i++)
        v[i + 1] = (SDL_Vertex){ pt[i], fc, { 0, 0 } };
    for (int i = 0; i < N; i++) { /* a fan around the centre */
        idx[i * 3] = 0;
        idx[i * 3 + 1] = i + 1;
        idx[i * 3 + 2] = (i + 1) % N + 1;
    }
    SDL_RenderGeometry(u->r, NULL, v, N + 1, idx, N * 3);
}

void ft_ui_box(ft_ui *u, SDL_FRect r, bool on, bool hot, SDL_Color c)
{
    ft_ui_fill(u, r, on ? ft_ui_alpha(c, 0.9f) : hot ? (SDL_Color){ 60, 60, 70, 220 } : (SDL_Color){ 0, 0, 0, 170 });
    ft_ui_outline(u, r, on || hot ? (SDL_Color){ 255, 255, 255, 230 } : (SDL_Color){ 255, 255, 255, 70 });
}

void ft_ui_button(ft_ui *u, SDL_FRect r, const char *jp, int px, bool on, bool hot, SDL_Color c)
{
    ft_ui_box(u, r, on, hot, c);
    ft_ui_draw_text(u, jp, px, r.x + r.w / 2, r.y + r.h / 2, on || hot ? FT_UI_TEXT : ft_ui_alpha(FT_UI_TEXT, 0.85f),
                    true);
}

void ft_ui_item(ft_ui *u, SDL_FRect r, const ft_tex *label, bool sel, bool enabled)
{
    ft_ui_box(u, r, false, sel && enabled, FT_UI_ACCENT);
    SDL_Color ink = !enabled ? (SDL_Color){ 110, 110, 116, 255 } : sel ? FT_UI_TEXT : (SDL_Color){ 210, 210, 210, 255 };
    ft_ui_draw_tex(u, label, r.x + r.w / 2, r.y + r.h / 2, ink, true, r.w - 20);
}

/* ---- screen ------------------------------------------------------------ */

void ft_ui_backdrop(ft_ui *u)
{
    SDL_SetRenderDrawBlendMode(u->r, SDL_BLENDMODE_BLEND);
    ft_ui_fill(u, (SDL_FRect){ 0, 0, FT_UI_W, FT_UI_H }, (SDL_Color){ 0, 0, 0, 215 });
}

bool ft_ui_inside(SDL_FRect r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}
