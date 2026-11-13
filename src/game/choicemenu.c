#include "game/choicemenu.h"

#include "game/ui.h"

#include <stdlib.h>
#include <string.h>

#define MAX_ITEMS FT_INTERP_CHOICES
#define SCENE_CY 277.0f         /* middle of the 554-high CG */
#define TOP_MIN 20.0f
#define ITEM_H 44.0f
#define GAP 10.0f
#define MIN_W 360.0f
#define MAX_W 820.0f
#define TEXT_PX 24

typedef struct item {
    ft_tex label;
    SDL_FRect r;
} item;

struct ft_choicemenu {
    ft_ui ui;
    bool open;
    item item[MAX_ITEMS];
    int count, sel;
};

ft_choicemenu *ft_choicemenu_create(SDL_Renderer *r)
{
    ft_choicemenu *m = calloc(1, sizeof *m);
    if (m)
        m->ui.r = r;
    return m;
}

void ft_choicemenu_destroy(ft_choicemenu *m)
{
    if (!m)
        return;
    ft_choicemenu_close(m);
    free(m);
}

/*
 * The text of a %cmnd entry, "verb[.call][.N]: label" (interp.c). The label is the jump target:
 * for look / talk / think it's place・action (下駄箱・見る), so untranslated ones show the action.
 */
static void command_text(const ft_ui *u, const char *text, char *out, size_t cap)
{
    const char *sep = strstr(text, ": ");
    const char *label = sep ? sep + 2 : text;
    const char *tr = ft_i18n_text(u->i18n, label);
    if (tr) {
        snprintf(out, cap, "%s", tr);
        return;
    }
    const char *last = NULL;
    for (const char *p = strstr(label, "・"); p; p = strstr(p + 1, "・"))
        last = p;
    bool action = strncmp(text, "move", 4) != 0 && strncmp(text, "system", 6) != 0;
    snprintf(out, cap, "%s", action && last && last[3] ? last + 3 : label);
}

void ft_choicemenu_open(ft_choicemenu *m, ft_choice_kind kind, const ft_choice *c, int count, const ft_i18n *i18n)
{
    if (!m)
        return;
    ft_choicemenu_close(m);
    m->ui.i18n = i18n;
    m->count = SDL_clamp(count, 0, MAX_ITEMS);
    m->sel = 0;
    char text[256];
    float w = 0;
    for (int i = 0; i < m->count; i++) {
        if (kind == FT_CHOICE_COMMAND)
            command_text(&m->ui, c[i].text, text, sizeof text);
        else
            snprintf(text, sizeof text, "%s", ft_ui_tr(&m->ui, c[i].text));
        m->item[i].label = ft_ui_render_text(&m->ui, text, TEXT_PX);
        w = SDL_max(w, m->item[i].label.w);
    }
    /* a centred column over the CG */
    w = SDL_clamp(w + 80, MIN_W, MAX_W);
    float h = ITEM_H * (float)m->count + GAP * (float)(m->count - 1);
    float top = SDL_max(TOP_MIN, SCENE_CY - h / 2);
    for (int i = 0; i < m->count; i++)
        m->item[i].r = (SDL_FRect){ SDL_floorf((FT_UI_W - w) / 2), top + (ITEM_H + GAP) * (float)i, w, ITEM_H };
    m->open = m->count > 0;
}

void ft_choicemenu_close(ft_choicemenu *m)
{
    if (!m)
        return;
    for (int i = 0; i < MAX_ITEMS; i++)
        ft_tex_free(&m->item[i].label);
    m->count = 0;
    m->open = false;
}

bool ft_choicemenu_is_open(const ft_choicemenu *m)
{
    return m && m->open;
}

static int hit(const ft_choicemenu *m, float x, float y)
{
    for (int i = 0; i < m->count; i++)
        if (ft_ui_inside(m->item[i].r, x, y))
            return i;
    return -1;
}

int ft_choicemenu_event(ft_choicemenu *m, const SDL_Event *ev)
{
    if (!m || !m->open)
        return -1;
    float x, y;
    int i;
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN: {
        SDL_Keycode k = ev->key.key;
        if (k == SDLK_UP)
            m->sel = (m->sel + m->count - 1) % m->count;
        else if (k == SDLK_DOWN || k == SDLK_TAB)
            m->sel = (m->sel + 1) % m->count;
        else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE)
            return m->sel;
        else if (k >= SDLK_1 && k <= SDLK_9 && (int)(k - SDLK_1) < m->count)
            return m->sel = (int)(k - SDLK_1);
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        SDL_RenderCoordinatesFromWindow(m->ui.r, ev->motion.x, ev->motion.y, &x, &y);
        if ((i = hit(m, x, y)) >= 0)
            m->sel = i;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        SDL_RenderCoordinatesFromWindow(m->ui.r, ev->button.x, ev->button.y, &x, &y);
        if (ev->button.button == SDL_BUTTON_LEFT && (i = hit(m, x, y)) >= 0)
            return m->sel = i;
        break;
    default:
        break;
    }
    return -1;
}

void ft_choicemenu_render(ft_choicemenu *m)
{
    if (!m || !m->open)
        return;
    SDL_SetRenderDrawBlendMode(m->ui.r, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < m->count; i++)
        ft_ui_item(&m->ui, m->item[i].r, &m->item[i].label, i == m->sel, true);
}
