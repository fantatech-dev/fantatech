#include "game/settingspanel.h"

#include "game/ui.h"

#include <stdlib.h>
#include <string.h>

/*
 * Rows, for the keyboard: one per language, then the four volumes, fullscreen, then the back
 * button. Languages are a vertical list in the left column; the volumes and, below them, the
 * display setting are in the right one.
 */

/* layout, in the 1024x768 logical screen */
#define MARGIN 56.0f
#define TITLE_Y 140.0f          /* column titles */
#define LIST_Y 176.0f           /* first row of each column */
#define LANG_X 72.0f
#define LANG_W 360.0f
#define LANG_H 46.0f
#define LANG_STEP 54.0f
#define VOL_X 472.0f
#define VOL_W (FT_UI_W - 72.0f - VOL_X)
#define VOL_H 56.0f
#define VOL_STEP 68.0f
#define BAR_X (VOL_X + 196.0f)
#define BAR_W 196.0f
#define DISP_TITLE_Y (LIST_Y + VOL_STEP * FT_VOL_COUNT + 28.0f) /* 画面 */
#define DISP_Y (DISP_TITLE_Y + 36.0f)
#define SEGMENTS 20             /* 5% each */
#define STEP 5

static const char *const volume_names[FT_VOL_COUNT] = { "マスター", "音楽", "音声", "効果音" };

struct ft_settingspanel {
    ft_ui ui;
    bool open;
    ft_config cfg;
    ft_lang_option lang[FT_SETTINGS_LANGS];
    ft_tex lang_tex[FT_SETTINGS_LANGS];
    int langs;
    int row;                    /* keyboard row */
    int hot_row;                /* row under the mouse, or -1 */
    bool hot_back;
    int drag;                   /* volume being dragged, or -1 */
};

static int rows(const ft_settingspanel *p)
{
    return p->langs + FT_VOL_COUNT + 2;
}

static int vol_row(const ft_settingspanel *p, int v)
{
    return p->langs + v;
}

static int disp_row(const ft_settingspanel *p)
{
    return p->langs + FT_VOL_COUNT;
}

static int back_row(const ft_settingspanel *p)
{
    return p->langs + FT_VOL_COUNT + 1;
}

/* The volume on `row`, or -1. */
static int row_volume(const ft_settingspanel *p, int row)
{
    return row >= p->langs && row < disp_row(p) ? row - p->langs : -1;
}

static SDL_FRect back_rect(void)
{
    SDL_FRect r = { FT_UI_W - MARGIN - 148, 702, 148, 42 };
    return r;
}

static SDL_FRect lang_rect(int i)
{
    SDL_FRect r = { LANG_X, LIST_Y + LANG_STEP * (float)i, LANG_W, LANG_H };
    return r;
}

static SDL_FRect vol_rect(int v)
{
    SDL_FRect r = { VOL_X, LIST_Y + VOL_STEP * (float)v - (VOL_H - LANG_H) / 2, VOL_W, VOL_H };
    return r;
}

static SDL_FRect bar_rect(int v)
{
    SDL_FRect b = vol_rect(v);
    SDL_FRect r = { BAR_X, b.y + (VOL_H - 30) / 2, BAR_W, 30 };
    return r;
}

static SDL_FRect disp_rect(void)
{
    SDL_FRect r = { VOL_X, DISP_Y, VOL_W, LANG_H };
    return r;
}

static SDL_FRect row_rect(const ft_settingspanel *p, int row)
{
    if (row == disp_row(p))
        return disp_rect();
    return row < p->langs ? lang_rect(row) : vol_rect(row - p->langs);
}

static int current_lang(const ft_settingspanel *p)
{
    for (int i = 0; i < p->langs; i++)
        if (strcmp(p->lang[i].id, p->cfg.lang) == 0)
            return i;
    return -1;
}

/* The names at 22 px, or smaller when one is too long for its row ("English (Machine Translation)"). */
static void layout_langs(ft_settingspanel *p)
{
    const float room = LANG_W - 64;
    for (int i = 0; i < p->langs; i++) {
        ft_tex_free(&p->lang_tex[i]);
        p->lang_tex[i] = ft_ui_render_text(&p->ui, p->lang[i].name, 22);
        if (p->lang_tex[i].w > room) {
            int px = SDL_max(14, (int)(22.0f * room / p->lang_tex[i].w));
            ft_tex_free(&p->lang_tex[i]);
            p->lang_tex[i] = ft_ui_render_text(&p->ui, p->lang[i].name, px);
        }
    }
}

/* ---- lifecycle --------------------------------------------------------- */

ft_settingspanel *ft_settingspanel_create(SDL_Renderer *r)
{
    ft_settingspanel *p = calloc(1, sizeof *p);
    if (p)
        p->ui.r = r;
    return p;
}

void ft_settingspanel_destroy(ft_settingspanel *p)
{
    if (!p)
        return;
    ft_settingspanel_close(p);
    free(p);
}

void ft_settingspanel_open(ft_settingspanel *p, const ft_config *cfg, const ft_lang_option *langs, int count,
                           const ft_i18n *i18n)
{
    if (!p)
        return;
    ft_settingspanel_close(p);
    p->open = true;
    p->cfg = *cfg;
    p->ui.i18n = i18n;
    p->langs = SDL_min(count, FT_SETTINGS_LANGS);
    memcpy(p->lang, langs, sizeof *langs * (size_t)p->langs);
    int cur = current_lang(p);
    p->row = cur >= 0 ? cur : vol_row(p, 0);
    p->hot_row = -1;
    p->hot_back = false;
    p->drag = -1;
    layout_langs(p);
}

void ft_settingspanel_close(ft_settingspanel *p)
{
    if (!p)
        return;
    for (int i = 0; i < FT_SETTINGS_LANGS; i++)
        ft_tex_free(&p->lang_tex[i]);
    ft_ui_clear(&p->ui);
    p->open = false;
}

bool ft_settingspanel_is_open(const ft_settingspanel *p)
{
    return p && p->open;
}

const ft_config *ft_settingspanel_config(const ft_settingspanel *p)
{
    return &p->cfg;
}

void ft_settingspanel_set_fullscreen(ft_settingspanel *p, bool on)
{
    if (p && p->open)
        p->cfg.fullscreen = on;
}

void ft_settingspanel_retranslate(ft_settingspanel *p)
{
    if (!p || !p->open)
        return;
    ft_ui_clear(&p->ui);
    layout_langs(p); /* a table's @font may have changed the font */
}

/* ---- input ------------------------------------------------------------- */

static ft_settings_action set_lang(ft_settingspanel *p, int i)
{
    if (i < 0 || i >= p->langs || strcmp(p->lang[i].id, p->cfg.lang) == 0)
        return FT_SETTINGS_NONE;
    snprintf(p->cfg.lang, sizeof p->cfg.lang, "%s", p->lang[i].id);
    return FT_SETTINGS_LANGUAGE;
}

static ft_settings_action set_volume(ft_settingspanel *p, int v, int value)
{
    value = SDL_clamp(value, 0, 100);
    if (p->cfg.volume[v] == value)
        return FT_SETTINGS_NONE;
    p->cfg.volume[v] = value;
    return FT_SETTINGS_VOLUME;
}

static ft_settings_action toggle_fullscreen(ft_settingspanel *p)
{
    p->cfg.fullscreen = !p->cfg.fullscreen;
    return FT_SETTINGS_DISPLAY;
}

/* Left / right: a volume step. */
static ft_settings_action adjust(ft_settingspanel *p, int dir)
{
    int v = row_volume(p, p->row);
    return v >= 0 ? set_volume(p, v, (p->cfg.volume[v] / STEP + dir) * STEP) : FT_SETTINGS_NONE;
}

static ft_settings_action key(ft_settingspanel *p, SDL_Keycode k)
{
    int n = rows(p);
    switch (k) {
    case SDLK_UP: p->row = (p->row + n - 1) % n; break;
    case SDLK_DOWN: case SDLK_TAB: p->row = (p->row + 1) % n; break;
    case SDLK_LEFT: return adjust(p, -1);
    case SDLK_RIGHT: return adjust(p, 1);
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
        if (p->row == back_row(p))
            return FT_SETTINGS_CLOSE;
        if (p->row == disp_row(p))
            return toggle_fullscreen(p);
        return p->row < p->langs ? set_lang(p, p->row) : adjust(p, 1);
    case SDLK_ESCAPE: case SDLK_BACKSPACE: return FT_SETTINGS_CLOSE;
    default: break;
    }
    return FT_SETTINGS_NONE;
}

static int bar_value(int v, float x)
{
    SDL_FRect b = bar_rect(v);
    return (int)SDL_lroundf((x - b.x) / b.w * SEGMENTS) * STEP;
}

static void hit_test(ft_settingspanel *p, float x, float y)
{
    p->hot_row = -1;
    p->hot_back = ft_ui_inside(back_rect(), x, y);
    for (int r = 0; r < back_row(p); r++)
        if (ft_ui_inside(row_rect(p, r), x, y))
            p->hot_row = r;
    if (p->hot_row >= 0)
        p->row = p->hot_row;
    else if (p->hot_back)
        p->row = back_row(p);
}

ft_settings_action ft_settingspanel_event(ft_settingspanel *p, const SDL_Event *ev)
{
    if (!p || !p->open)
        return FT_SETTINGS_NONE;
    float x, y;
    int v;
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
        return key(p, ev->key.key);
    case SDL_EVENT_MOUSE_MOTION:
        SDL_RenderCoordinatesFromWindow(p->ui.r, ev->motion.x, ev->motion.y, &x, &y);
        if (p->drag >= 0)
            return set_volume(p, p->drag, bar_value(p->drag, x));
        hit_test(p, x, y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        SDL_RenderCoordinatesFromWindow(p->ui.r, ev->button.x, ev->button.y, &x, &y);
        hit_test(p, x, y);
        if (ev->button.button == SDL_BUTTON_RIGHT)
            return FT_SETTINGS_CLOSE;
        if (ev->button.button != SDL_BUTTON_LEFT)
            break;
        if (p->hot_back)
            return FT_SETTINGS_CLOSE;
        if (p->hot_row >= 0 && p->hot_row < p->langs)
            return set_lang(p, p->hot_row);
        if (p->hot_row == disp_row(p))
            return toggle_fullscreen(p);
        if ((v = row_volume(p, p->hot_row)) >= 0) { /* click or drag along the slider */
            SDL_FRect b = bar_rect(v);
            if (x >= b.x - 20 && x <= b.x + b.w + 20) {
                p->drag = v;
                return set_volume(p, v, bar_value(v, x));
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        p->drag = -1;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (row_volume(p, p->row) >= 0 && ev->wheel.y != 0)
            return adjust(p, ev->wheel.y > 0 ? 1 : -1);
        break;
    default:
        break;
    }
    return FT_SETTINGS_NONE;
}

/* ---- drawing ----------------------------------------------------------- */

/* The keyboard row: a faint band. */
static void highlight(ft_settingspanel *p, SDL_FRect b)
{
    ft_ui_fill(&p->ui, b, (SDL_Color){ 255, 255, 255, 22 });
    ft_ui_outline(&p->ui, b, (SDL_Color){ 255, 255, 255, 60 });
}

/* One language: a radio marker, filled for the current one, and its name. */
static void draw_lang(ft_settingspanel *p, int i, bool current)
{
    SDL_FRect r = lang_rect(i);
    bool sel = p->row == i;
    if (sel)
        highlight(p, r);
    float cy = r.y + r.h / 2, cx = r.x + 26;
    ft_ui_circle(&p->ui, cx, cy, 10, false, current || sel ? FT_UI_TEXT : FT_UI_DIM);
    if (current)
        ft_ui_circle(&p->ui, cx, cy, 6, true, FT_UI_ACCENT);
    ft_ui_draw_tex(&p->ui, &p->lang_tex[i], r.x + 52, cy, current || sel ? FT_UI_TEXT : ft_ui_alpha(FT_UI_TEXT, 0.75f),
                   false, r.w - 64);
}

/* A slider: the track, filled up to the value, and a knob. */
static void draw_volume(ft_settingspanel *p, int v)
{
    bool sel = p->row == vol_row(p, v);
    SDL_FRect band = vol_rect(v);
    if (sel)
        highlight(p, band);
    float cy = band.y + band.h / 2;
    ft_ui_draw_text(&p->ui, volume_names[v], 22, band.x + 20, cy, sel ? FT_UI_TEXT : ft_ui_alpha(FT_UI_TEXT, 0.85f),
                    false);

    SDL_FRect bar = bar_rect(v);
    float fill = bar.w * (float)p->cfg.volume[v] / 100.0f;
    ft_ui_fill(&p->ui, (SDL_FRect){ bar.x, cy - 3, bar.w, 6 }, FT_UI_FAINT);
    ft_ui_fill(&p->ui, (SDL_FRect){ bar.x, cy - 3, fill, 6 }, FT_UI_ACCENT);
    SDL_FRect knob = { bar.x + fill - 7, cy - 12, 14, 24 };
    ft_ui_fill(&p->ui, knob, sel ? FT_UI_TEXT : (SDL_Color){ 200, 200, 204, 255 });
    ft_ui_outline(&p->ui, knob, FT_UI_BLACK);
    char value[8];
    snprintf(value, sizeof value, "%d%%", p->cfg.volume[v]);
    ft_ui_draw_label(&p->ui, value, 22, bar.x + bar.w + 18, cy, sel ? FT_UI_TEXT : FT_UI_DIM, false);
}

/* Fullscreen: a check box and its label. */
static void draw_display(ft_settingspanel *p)
{
    SDL_FRect r = disp_rect();
    bool sel = p->row == disp_row(p), on = p->cfg.fullscreen;
    if (sel)
        highlight(p, r);
    float cy = r.y + r.h / 2;
    SDL_FRect box = { r.x + 16, cy - 10, 20, 20 };
    ft_ui_outline(&p->ui, box, on || sel ? FT_UI_TEXT : FT_UI_DIM);
    if (on)
        ft_ui_fill(&p->ui, (SDL_FRect){ box.x + 5, box.y + 5, box.w - 10, box.h - 10 }, FT_UI_ACCENT);
    ft_ui_draw_text(&p->ui, "フルスクリーン", 22, r.x + 52, cy, sel ? FT_UI_TEXT : ft_ui_alpha(FT_UI_TEXT, 0.85f),
                    false);
}

void ft_settingspanel_render(ft_settingspanel *p)
{
    if (!p || !p->open)
        return;
    ft_ui_backdrop(&p->ui);
    ft_ui_draw_text(&p->ui, "システム設定", 32, MARGIN, 62, FT_UI_TEXT, false);
    ft_ui_fill(&p->ui, (SDL_FRect){ MARGIN, 94, FT_UI_W - 2 * MARGIN, 1 }, FT_UI_FAINT);

    ft_ui_draw_text(&p->ui, "言語", 26, LANG_X + 20, TITLE_Y, FT_UI_TEXT, false);
    int cur = current_lang(p);
    for (int i = 0; i < p->langs; i++)
        draw_lang(p, i, i == cur);

    ft_ui_fill(&p->ui, (SDL_FRect){ VOL_X - 20, TITLE_Y - 20, 1, DISP_Y + LANG_H - TITLE_Y + 20 }, FT_UI_FAINT);
    ft_ui_draw_text(&p->ui, "音量", 26, VOL_X + 20, TITLE_Y, FT_UI_TEXT, false);
    for (int v = 0; v < FT_VOL_COUNT; v++)
        draw_volume(p, v);
    ft_ui_draw_text(&p->ui, "画面", 26, VOL_X + 20, DISP_TITLE_Y, FT_UI_TEXT, false);
    draw_display(p);

    /* footer */
    ft_ui_fill(&p->ui, (SDL_FRect){ MARGIN, 690, FT_UI_W - 2 * MARGIN, 1 }, FT_UI_FAINT);
    ft_ui_draw_text(&p->ui, "↑↓：項目　Enter：決定　←→：音量　Esc：戻る", 16, MARGIN,
                    back_rect().y + back_rect().h / 2, FT_UI_DIM, false);
    ft_ui_button(&p->ui, back_rect(), "戻る", 22, p->row == back_row(p), p->hot_back, FT_UI_ACCENT);
}
