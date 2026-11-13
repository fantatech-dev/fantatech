#include "game/titlemenu.h"

#include "game/ui.h"

#include <stdlib.h>
#include <string.h>

/* layout, in the 1024x768 logical screen: a column centred on x, at the height the runtime asks */
#define CX 512.0f
#define ITEM_W 340.0f
#define ITEM_H 46.0f
#define ITEM_GAP 10.0f

typedef struct item {
    ft_tex label;
    bool enabled;
} item;

struct ft_titlemenu {
    ft_ui ui;
    bool open;
    item item[FT_TITLEMENU_ITEMS];
    int count, sel;
    float cy;
};

static SDL_FRect item_rect(const ft_titlemenu *m, int i)
{
    float total = ITEM_H * (float)m->count + ITEM_GAP * (float)(m->count - 1);
    SDL_FRect r = { CX - ITEM_W / 2, m->cy - total / 2 + (ITEM_H + ITEM_GAP) * (float)i, ITEM_W, ITEM_H };
    return r;
}

ft_titlemenu *ft_titlemenu_create(SDL_Renderer *r)
{
    ft_titlemenu *m = calloc(1, sizeof *m);
    if (m)
        m->ui.r = r;
    return m;
}

void ft_titlemenu_destroy(ft_titlemenu *m)
{
    if (!m)
        return;
    ft_titlemenu_close(m);
    free(m);
}

void ft_titlemenu_open(ft_titlemenu *m, const char *const *texts, const bool *enabled, int count, float cy,
                       const ft_i18n *i18n)
{
    if (!m)
        return;
    ft_titlemenu_close(m);
    m->cy = cy;
    m->ui.i18n = i18n;
    m->count = SDL_min(count, FT_TITLEMENU_ITEMS);
    m->sel = -1;
    for (int i = 0; i < m->count; i++) {
        item *it = &m->item[i];
        it->label = ft_ui_render_text(&m->ui, ft_ui_tr(&m->ui, texts[i]), 24);
        it->enabled = !enabled || enabled[i];
        if (m->sel < 0 && it->enabled)
            m->sel = i;
    }
    m->open = true;
}

void ft_titlemenu_close(ft_titlemenu *m)
{
    if (!m)
        return;
    for (int i = 0; i < FT_TITLEMENU_ITEMS; i++)
        ft_tex_free(&m->item[i].label);
    m->open = false;
}

bool ft_titlemenu_is_open(const ft_titlemenu *m)
{
    return m && m->open;
}

int ft_titlemenu_selected(const ft_titlemenu *m)
{
    return m->sel;
}

void ft_titlemenu_select(ft_titlemenu *m, int i)
{
    if (m && i >= 0 && i < m->count && m->item[i].enabled)
        m->sel = i;
}

/* The next available entry from the selection in direction `dir`, wrapping. */
static void move(ft_titlemenu *m, int dir)
{
    for (int k = 1; k <= m->count; k++) {
        int i = ((m->sel < 0 ? 0 : m->sel) + dir * k + m->count * k) % m->count;
        if (m->item[i].enabled) {
            m->sel = i;
            return;
        }
    }
}

static int hit(const ft_titlemenu *m, float x, float y)
{
    for (int i = 0; i < m->count; i++)
        if (m->item[i].enabled && ft_ui_inside(item_rect(m, i), x, y))
            return i;
    return -1;
}

int ft_titlemenu_event(ft_titlemenu *m, const SDL_Event *ev)
{
    if (!m || !m->open)
        return -1;
    float x, y;
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
        switch (ev->key.key) {
        case SDLK_UP: move(m, -1); break;
        case SDLK_DOWN: case SDLK_TAB: move(m, 1); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return m->sel;
        default: break;
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        SDL_RenderCoordinatesFromWindow(m->ui.r, ev->motion.x, ev->motion.y, &x, &y);
        if (hit(m, x, y) >= 0)
            m->sel = hit(m, x, y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        SDL_RenderCoordinatesFromWindow(m->ui.r, ev->button.x, ev->button.y, &x, &y);
        if (ev->button.button == SDL_BUTTON_LEFT && hit(m, x, y) >= 0)
            return m->sel = hit(m, x, y);
        break;
    default:
        break;
    }
    return -1;
}

void ft_titlemenu_render(ft_titlemenu *m)
{
    if (!m || !m->open)
        return;
    SDL_SetRenderDrawBlendMode(m->ui.r, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < m->count; i++)
        ft_ui_item(&m->ui, item_rect(m, i), &m->item[i].label, i == m->sel, m->item[i].enabled);
}
