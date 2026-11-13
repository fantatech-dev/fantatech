#include "game/savepanel.h"

#include "game/save.h"
#include "game/ui.h"

#include <stdlib.h>
#include <string.h>

#define PER_PAGE 8
#define PAGES (FT_SAVE_SLOTS / PER_PAGE)

/* layout, in the 1024x768 logical screen */
#define MARGIN 56.0f
#define CARD_W 440.0f
#define CARD_H 132.0f
#define CARD_Y0 104.0f
#define CARD_STEP 146.0f
#define THUMB_W 192.0f
#define THUMB_H 120.0f
#define TEXT_X 212.0f           /* card text, from the card's left */
#define RIGHT_COL (1024.0f - MARGIN - CARD_W)
#define PAGE_BTN 42.0f
#define PAGE_GAP 8.0f

enum { HOT_NONE, HOT_CARD, HOT_DELETE, HOT_TAB_SAVE, HOT_TAB_LOAD, HOT_PAGE, HOT_SETTINGS, HOT_BACK, HOT_YES, HOT_NO };

typedef struct slot_view {
    bool used;
    int64_t time;
    ft_tex thumb, title, date;
} slot_view;

struct ft_savepanel {
    ft_ui ui;
    char dir[1024];
    bool open, can_save, saving;
    int page;
    int sel;                    /* card on the page, 0..7, or -1 */
    int hot, hot_arg;           /* control under the mouse */
    slot_view slot[FT_SAVE_SLOTS];

    ft_panel_action confirm;    /* asking before this action, or NONE */
    int confirm_slot;           /* 0-based */
    bool confirm_yes;           /* the highlighted answer */

    char notice[128];
    uint64_t notice_ms;
};

/* ---- slots ------------------------------------------------------------- */

static void slot_free(slot_view *s)
{
    ft_tex_free(&s->thumb);
    ft_tex_free(&s->title);
    ft_tex_free(&s->date);
    s->used = false;
}

void ft_savepanel_refresh(ft_savepanel *p, int slot)
{
    if (slot < 1 || slot > FT_SAVE_SLOTS)
        return;
    slot_view *v = &p->slot[slot - 1];
    slot_free(v);
    char path[1100];
    ft_save_path(path, sizeof path, p->dir, slot, "sav");
    ft_save s;
    if (!ft_save_read(path, &s, false))
        return;
    v->used = true;
    v->time = s.time;
    v->title = ft_ui_render_text(&p->ui, s.title[0] ? ft_ui_tr(&p->ui, s.title) : s.frame[s.frames - 1].script, 24);
    SDL_DateTime dt;
    if (SDL_TimeToDateTime((SDL_Time)s.time * SDL_NS_PER_SECOND, &dt, true)) {
        char date[64];
        snprintf(date, sizeof date, "%04d.%02d.%02d  %02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute);
        v->date = ft_ui_render_text(&p->ui, date, 16);
    }
    ft_save_free(&s);
    ft_save_path(path, sizeof path, p->dir, slot, "png");
    SDL_Surface *img = SDL_LoadPNG(path);
    if (img) {
        v->thumb.t = SDL_CreateTextureFromSurface(p->ui.r, img);
        if (v->thumb.t) {
            SDL_SetTextureScaleMode(v->thumb.t, SDL_SCALEMODE_LINEAR);
            v->thumb.w = (float)img->w;
            v->thumb.h = (float)img->h;
        }
        SDL_DestroySurface(img);
    }
}

/* ---- lifecycle --------------------------------------------------------- */

ft_savepanel *ft_savepanel_create(SDL_Renderer *r, const char *dir)
{
    ft_savepanel *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->ui.r = r;
    snprintf(p->dir, sizeof p->dir, "%s", dir);
    p->sel = 0;
    return p;
}

void ft_savepanel_destroy(ft_savepanel *p)
{
    if (!p)
        return;
    ft_savepanel_close(p);
    free(p);
}

void ft_savepanel_open(ft_savepanel *p, bool can_save, const ft_i18n *i18n)
{
    if (!p)
        return;
    ft_savepanel_close(p);
    p->ui.i18n = i18n;
    p->open = true;
    p->can_save = can_save;
    p->saving = can_save;
    p->confirm = FT_PANEL_NONE;
    p->hot = HOT_NONE;
    p->notice[0] = 0;
    if (p->sel < 0)
        p->sel = 0;
    int newest = -1;
    for (int i = 1; i <= FT_SAVE_SLOTS; i++) {
        ft_savepanel_refresh(p, i);
        if (p->slot[i - 1].used && (newest < 0 || p->slot[i - 1].time > p->slot[newest].time))
            newest = i - 1;
    }
    if (!can_save && newest >= 0) { /* continuing from the title: the latest save */
        p->page = newest / PER_PAGE;
        p->sel = newest % PER_PAGE;
    }
}

void ft_savepanel_close(ft_savepanel *p)
{
    if (!p)
        return;
    for (int i = 0; i < FT_SAVE_SLOTS; i++)
        slot_free(&p->slot[i]);
    ft_ui_clear(&p->ui);
    p->open = false;
}

bool ft_savepanel_is_open(const ft_savepanel *p)
{
    return p && p->open;
}

void ft_savepanel_retranslate(ft_savepanel *p)
{
    if (!p || !p->open)
        return;
    ft_ui_clear(&p->ui);
    for (int i = 1; i <= FT_SAVE_SLOTS; i++)
        ft_savepanel_refresh(p, i);
}

void ft_savepanel_notice(ft_savepanel *p, const char *text)
{
    snprintf(p->notice, sizeof p->notice, "%s", text);
    p->notice_ms = SDL_GetTicks();
}

/* ---- geometry ---------------------------------------------------------- */

static SDL_FRect card_rect(int i)
{
    SDL_FRect r = { i % 2 ? RIGHT_COL : MARGIN, CARD_Y0 + CARD_STEP * (float)(i / 2), CARD_W, CARD_H };
    return r;
}

static SDL_FRect delete_rect(int i)
{
    SDL_FRect c = card_rect(i);
    SDL_FRect r = { c.x + c.w - 92, c.y + 10, 80, 26 }; /* top right, clear of the title and date */
    return r;
}

static SDL_FRect tab_rect(int i)
{
    SDL_FRect r = { MARGIN + 156 * (float)i, 40, 148, 44 };
    return r;
}

static SDL_FRect page_rect(int i)
{
    float right = 1024 - MARGIN;
    SDL_FRect r = { right - (PAGE_BTN + PAGE_GAP) * (float)(PAGES - i) + PAGE_GAP, 42, PAGE_BTN, PAGE_BTN };
    return r;
}

static SDL_FRect back_rect(void)
{
    SDL_FRect r = { 1024 - MARGIN - 148, 702, 148, 42 };
    return r;
}

static SDL_FRect settings_rect(void)
{
    SDL_FRect r = back_rect();
    r.x -= r.w + 12;
    return r;
}

static SDL_FRect dialog_rect(void)
{
    SDL_FRect r = { 512 - 280, 384 - 110, 560, 220 };
    return r;
}

static SDL_FRect answer_rect(bool yes)
{
    SDL_FRect d = dialog_rect();
    SDL_FRect r = { d.x + (yes ? 110 : 300), d.y + 150, 150, 44 };
    return r;
}


/* ---- input ------------------------------------------------------------- */

static int slot_index(const ft_savepanel *p)
{
    return p->page * PER_PAGE + (p->sel < 0 ? 0 : p->sel);
}

static void hit_test(ft_savepanel *p, float x, float y)
{
    p->hot = HOT_NONE;
    if (p->confirm) {
        if (ft_ui_inside(answer_rect(true), x, y))
            p->hot = HOT_YES, p->confirm_yes = true;
        else if (ft_ui_inside(answer_rect(false), x, y))
            p->hot = HOT_NO, p->confirm_yes = false;
        return;
    }
    for (int i = 0; i < PER_PAGE; i++)
        if (ft_ui_inside(card_rect(i), x, y)) {
            p->sel = i;
            p->hot = p->slot[p->page * PER_PAGE + i].used && ft_ui_inside(delete_rect(i), x, y) ? HOT_DELETE : HOT_CARD;
            return;
        }
    for (int i = 0; i < 2 && p->can_save; i++)
        if (ft_ui_inside(tab_rect(i), x, y))
            p->hot = i ? HOT_TAB_LOAD : HOT_TAB_SAVE;
    for (int i = 0; i < PAGES; i++)
        if (ft_ui_inside(page_rect(i), x, y))
            p->hot = HOT_PAGE, p->hot_arg = i;
    if (ft_ui_inside(back_rect(), x, y))
        p->hot = HOT_BACK;
    if (ft_ui_inside(settings_rect(), x, y))
        p->hot = HOT_SETTINGS;
}

static ft_panel_action ask(ft_savepanel *p, ft_panel_action a, int index)
{
    p->confirm = a;
    p->confirm_slot = index;
    p->confirm_yes = a != FT_PANEL_DELETE; /* deleting defaults to no */
    return FT_PANEL_NONE;
}

/* Enter / click on the selected card. */
static ft_panel_action activate(ft_savepanel *p, int *slot)
{
    int i = slot_index(p);
    *slot = i + 1;
    if (p->saving)
        return p->slot[i].used ? ask(p, FT_PANEL_SAVE, i) : FT_PANEL_SAVE;
    if (!p->slot[i].used)
        return FT_PANEL_NONE;
    return p->can_save ? ask(p, FT_PANEL_LOAD, i) : FT_PANEL_LOAD; /* in a game: progress would be lost */
}

static ft_panel_action delete_selected(ft_savepanel *p)
{
    int i = slot_index(p);
    return p->slot[i].used ? ask(p, FT_PANEL_DELETE, i) : FT_PANEL_NONE;
}

static ft_panel_action answer(ft_savepanel *p, bool yes, int *slot)
{
    ft_panel_action a = p->confirm;
    p->confirm = FT_PANEL_NONE;
    p->hot = HOT_NONE;
    *slot = p->confirm_slot + 1;
    return yes ? a : FT_PANEL_NONE;
}

static void set_page(ft_savepanel *p, int page)
{
    p->page = (page % PAGES + PAGES) % PAGES;
}

static void move_sel(ft_savepanel *p, int dx, int dy)
{
    if (p->sel < 0)
        p->sel = 0;
    int col = p->sel % 2, row = p->sel / 2;
    if (dx) { /* off the side: the neighbouring page */
        col += dx;
        if (col < 0 || col > 1) {
            set_page(p, p->page + dx);
            col = col < 0 ? 1 : 0;
        }
    }
    row = (row + dy + PER_PAGE / 2) % (PER_PAGE / 2);
    p->sel = row * 2 + col;
}

static ft_panel_action key(ft_savepanel *p, SDL_Keycode k, int *slot)
{
    if (p->confirm) {
        if (k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_TAB)
            p->confirm_yes = !p->confirm_yes;
        else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE)
            return answer(p, p->confirm_yes, slot);
        else if (k == SDLK_Y)
            return answer(p, true, slot);
        else if (k == SDLK_N || k == SDLK_ESCAPE || k == SDLK_BACKSPACE)
            return answer(p, false, slot);
        return FT_PANEL_NONE;
    }
    switch (k) {
    case SDLK_LEFT: move_sel(p, -1, 0); break;
    case SDLK_RIGHT: move_sel(p, 1, 0); break;
    case SDLK_UP: move_sel(p, 0, -1); break;
    case SDLK_DOWN: move_sel(p, 0, 1); break;
    case SDLK_PAGEUP: case SDLK_Q: set_page(p, p->page - 1); break;
    case SDLK_PAGEDOWN: case SDLK_E: set_page(p, p->page + 1); break;
    case SDLK_TAB:
        if (p->can_save)
            p->saving = !p->saving;
        break;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return activate(p, slot);
    case SDLK_DELETE: case SDLK_BACKSPACE: return delete_selected(p);
    case SDLK_ESCAPE: return FT_PANEL_BACK;
    default:
        if (k >= SDLK_1 && k < SDLK_1 + PAGES)
            set_page(p, (int)(k - SDLK_1));
        break;
    }
    return FT_PANEL_NONE;
}

static ft_panel_action click(ft_savepanel *p, int *slot)
{
    switch (p->hot) {
    case HOT_CARD: return activate(p, slot);
    case HOT_DELETE: return delete_selected(p);
    case HOT_TAB_SAVE: p->saving = true; break;
    case HOT_TAB_LOAD: p->saving = false; break;
    case HOT_PAGE: set_page(p, p->hot_arg); break;
    case HOT_BACK: return FT_PANEL_BACK;
    case HOT_SETTINGS: return FT_PANEL_SETTINGS;
    case HOT_YES: return answer(p, true, slot);
    case HOT_NO: return answer(p, false, slot);
    default: break;
    }
    return FT_PANEL_NONE;
}

ft_panel_action ft_savepanel_event(ft_savepanel *p, const SDL_Event *ev, int *slot)
{
    *slot = 0;
    if (!p || !p->open)
        return FT_PANEL_NONE;
    float x, y;
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
        return key(p, ev->key.key, slot);
    case SDL_EVENT_MOUSE_MOTION:
        SDL_RenderCoordinatesFromWindow(p->ui.r, ev->motion.x, ev->motion.y, &x, &y);
        hit_test(p, x, y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        SDL_RenderCoordinatesFromWindow(p->ui.r, ev->button.x, ev->button.y, &x, &y);
        hit_test(p, x, y);
        if (ev->button.button == SDL_BUTTON_RIGHT) /* right click backs out, like Esc */
            return p->confirm ? answer(p, false, slot) : FT_PANEL_BACK;
        if (ev->button.button == SDL_BUTTON_LEFT)
            return click(p, slot);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!p->confirm && ev->wheel.y != 0)
            set_page(p, p->page + (ev->wheel.y < 0 ? 1 : -1));
        break;
    default:
        break;
    }
    return FT_PANEL_NONE;
}

/* ---- drawing ----------------------------------------------------------- */

static void draw_card(ft_savepanel *p, int i)
{
    int index = p->page * PER_PAGE + i;
    const slot_view *v = &p->slot[index];
    SDL_FRect r = card_rect(i);
    bool sel = i == p->sel;
    bool usable = v->used || p->saving;
    bool lit = sel && usable;
    ft_ui_box(&p->ui, r, false, lit, FT_UI_ACCENT);

    SDL_FRect th = { r.x + 8, r.y + 6, THUMB_W, THUMB_H };
    if (v->thumb.t)
        SDL_RenderTexture(p->ui.r, v->thumb.t, NULL, &th);
    else
        ft_ui_fill(&p->ui, th, (SDL_Color){ 24, 24, 28, 255 });
    ft_ui_outline(&p->ui, th, FT_UI_FAINT);

    char no[16];
    snprintf(no, sizeof no, "No.%02d", index + 1);
    ft_tex t = ft_ui_render_text(&p->ui, no, 16);
    ft_ui_draw_tex(&p->ui, &t, r.x + TEXT_X, r.y + 22, FT_UI_DIM, false, 0);
    ft_tex_free(&t);
    float text_w = CARD_W - TEXT_X - 14;
    if (v->used) {
        ft_ui_draw_tex(&p->ui, &v->title, r.x + TEXT_X, r.y + 60, lit ? FT_UI_TEXT : ft_ui_alpha(FT_UI_TEXT, 0.85f), false,
                       text_w);
        ft_ui_draw_tex(&p->ui, &v->date, r.x + TEXT_X, r.y + 98, FT_UI_DIM, false, text_w);
        if (sel)
            ft_ui_button(&p->ui, delete_rect(i), "削除", 16, p->hot == HOT_DELETE, true, FT_UI_RED);
    } else {
        ft_ui_draw_text(&p->ui, "空きスロット", 20, r.x + TEXT_X, r.y + 60, FT_UI_DIM, false);
    }
}

static void draw_dialog(ft_savepanel *p)
{
    static const char *const questions[] = { NULL, "このスロットに上書きしますか？", "このデータを読み込みますか？",
                                             "このデータを削除しますか？" };
    bool warn = p->confirm != FT_PANEL_LOAD; /* overwriting or deleting loses a save */
    ft_ui_fill(&p->ui, (SDL_FRect){ 0, 0, 1024, 768 }, (SDL_Color){ 0, 0, 0, 150 });
    SDL_FRect d = dialog_rect();
    ft_ui_fill(&p->ui, d, (SDL_Color){ 20, 20, 24, 245 });
    ft_ui_outline(&p->ui, d, (SDL_Color){ 255, 255, 255, 120 });
    ft_ui_draw_text(&p->ui, questions[p->confirm], 26, d.x + d.w / 2, d.y + 80, FT_UI_TEXT, true);
    ft_ui_button(&p->ui, answer_rect(true), "はい", 22, p->confirm_yes, p->hot == HOT_YES, warn ? FT_UI_RED : FT_UI_ACCENT);
    ft_ui_button(&p->ui, answer_rect(false), "いいえ", 22, !p->confirm_yes, p->hot == HOT_NO, FT_UI_ACCENT);
}

void ft_savepanel_render(ft_savepanel *p)
{
    if (!p || !p->open)
        return;
    /* header: mode tabs (or the load-only title), notice, page buttons */
    ft_ui_backdrop(&p->ui);
    if (p->can_save) {
        ft_ui_button(&p->ui, tab_rect(0), "セーブ", 24, p->saving, p->hot == HOT_TAB_SAVE, FT_UI_ACCENT);
        ft_ui_button(&p->ui, tab_rect(1), "ロード", 24, !p->saving, p->hot == HOT_TAB_LOAD, FT_UI_ACCENT);
    } else {
        SDL_FRect t = tab_rect(0);
        ft_ui_draw_text(&p->ui, "ロード", 32, t.x, t.y + t.h / 2, FT_UI_TEXT, false);
    }
    for (int i = 0; i < PAGES; i++) {
        SDL_FRect r = page_rect(i);
        bool hot = p->hot == HOT_PAGE && p->hot_arg == i;
        char n[4];
        snprintf(n, sizeof n, "%d", i + 1);
        ft_ui_box(&p->ui, r, i == p->page, hot, FT_UI_ACCENT);
        ft_ui_draw_label(&p->ui, n, 20, r.x + r.w / 2, r.y + r.h / 2,
                         i == p->page || hot ? FT_UI_TEXT : ft_ui_alpha(FT_UI_TEXT, 0.75f), true);
    }
    if (p->notice[0] && SDL_GetTicks() - p->notice_ms < 1800) {
        float x = (tab_rect(1).x + tab_rect(1).w + page_rect(0).x) / 2;
        ft_tex t = ft_ui_render_text(&p->ui, ft_ui_tr(&p->ui, p->notice), 20);
        SDL_FRect box = { x - t.w / 2 - 18, 44, t.w + 36, 36 };
        ft_ui_fill(&p->ui, box, (SDL_Color){ 20, 20, 24, 235 });
        ft_ui_outline(&p->ui, box, (SDL_Color){ 255, 255, 255, 120 });
        ft_ui_draw_tex(&p->ui, &t, x, 62, FT_UI_TEXT, true, 0);
        ft_tex_free(&t);
    }
    ft_ui_fill(&p->ui, (SDL_FRect){ MARGIN, 94, 1024 - 2 * MARGIN, 1 }, FT_UI_FAINT);

    for (int i = 0; i < PER_PAGE; i++)
        draw_card(p, i);

    /* footer: rule, key help and the back button */
    ft_ui_fill(&p->ui, (SDL_FRect){ MARGIN, 690, 1024 - 2 * MARGIN, 1 }, FT_UI_FAINT);
    ft_ui_draw_text(&p->ui, p->can_save ? "クリック・Enter：決定　Tab：セーブ／ロード切替　Del：削除　Esc：戻る"
                                        : "クリック・Enter：読み込む　Del：削除　Esc：戻る",
                    16, MARGIN, back_rect().y + back_rect().h / 2, FT_UI_DIM, false);
    ft_ui_button(&p->ui, settings_rect(), "設定", 22, false, p->hot == HOT_SETTINGS, FT_UI_ACCENT);
    ft_ui_button(&p->ui, back_rect(), "戻る", 22, false, p->hot == HOT_BACK, FT_UI_ACCENT);

    if (p->confirm)
        draw_dialog(p);
}
