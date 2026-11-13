#include "game/runtime.h"

#include "codec/wav.h"
#include "game/assets.h"
#include "game/choicemenu.h"
#include "game/config.h"
#include "game/font.h"
#include "game/i18n.h"
#include "game/movie.h"
#include "game/save.h"
#include "game/savepanel.h"
#include "game/settingspanel.h"
#include "game/titlemenu.h"
#include "script/interp.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/*
 * Layout, measured on a recording of the original in the VM (1024x768 shown at x1.25):
 * there is no message window and no speaker name, just the CG on black with the line
 * underneath.
 */
#define SCREEN_W 1024
#define SCREEN_H 768
#define SCENE_H 554             /* eve1024 CG height */
/*
 * Face slots are 0-based (%face 0..4). %faceFormation n sets each slot's x (FUN_0040f090,
 * +0x38c), which is the portrait's left edge:
 *   0/1: 0,2 -> 168  1,3 -> 664 (0/1 stacked above 2/3)     2: 0 -> 168  1 -> 664  2,3 -> 416
 *   3:   0,4 -> 416                                           4: 140, 248, 596, 704
 * With ktxp01.ini's FACE_SCALE=3 ("small") the portrait is 160x180 (the face tier-1 size)
 * with its top at y 432, over the CG's bottom edge. The original frames it with frmface1
 * (frame.lb5) drawn 188x199 from (x - 18, y - 9); we draw a plain border instead (draw_face_frame). y is camera maths in the original (FUN_0040ef90); slot 3
 * in formation 2 uses base 200 instead of 544, so it sits 344 higher.
 */
#define FACE_SLOTS 8
#define FACE_W 160.0f
#define FACE_H 180.0f
#define FACE_Y 432.0f
/*
 * The original's slots are centred on x 496 (centre slot 416..576, left and right mirrored about
 * it), 16 px left of the screen centre the dialogue line is centred on. We move them all right by
 * that much so the centre face sits over the text (our change, not the VM layout).
 */
#define FACE_SHIFT_X 16.0f
/*
 * Dialogue: the textbmp line image is drawn x1.30, centered on the screen, top at y 635
 * (fitted to the glyph boxes of a11006, a21003 and a21010 in the VM recording, within 1 px).
 */
#define TEXT_SCALE 1.30f
#define TEXT_Y 635.0f
/* Translated lines are drawn with the font at about the textbmp glyph size, wrapped and centred. */
#define TEXT_TR_PX 28
#define TEXT_TR_WRAP 960
#define FONT_PX 24
/*
 * Films (kt_cmd_film 0x424d10) advance n/14 frames per 14 ms timer tick, n from
 * %film.Timer / SpeedThreadTimer (0..60, FUN_00424c30), so a frame lasts 196/n ms.
 * ZDF CGs shown through %s / %event use the common n = 2.
 */
#define FILM_TICK_MS 14
#define FILM_DEFAULT_RATE 2
#define FILM_FRAME_MS (FILM_TICK_MS * 14 / FILM_DEFAULT_RATE)
#define FILM_PARTS 8
/*
 * Script clock: mana01 runs timeSetEvent(14 ms, periodic) with callback 0x456550, which
 * adds 1.40 units per tick, so 1 unit = 10 ms. %timer.wait N blocks until the clock
 * reaches N; nothing the player does shortens it, except with OnKeyJump armed, where a
 * key jumps the clock to the target (0x5c2058, set only by OnKeyJump).
 */
#define TIMER_UNIT_MS 10
/*
 * Frames: the render thread runs a text3d script (dd01 / black / movie: `dd.render`,
 * `dd.wait`, loop), and dd.wait blocks on the event the 14 ms timer sets each tick
 * (kt_timer_tick -> 0x4562a0). So one frame per tick, and every per-frame animation
 * (fades, %sprite steps, %selSpeed, films) runs on this clock.
 */
#define FRAME_MS 14

/* save screenshots (the top THUMB_SRC_H rows: the CG and most of the faces), and the black that
   fades off the scene after a load (about 0.3 s) */
#define THUMB_SRC_H 640
#define THUMB_W 320
#define THUMB_H 200
#define CURTAIN_STEP 12

typedef struct layer {
    SDL_Texture *tex;
    float w, h;
    ft_blob blob;               /* kept for ZDF animation */
    ft_zdf zdf;
    bool animated;
    uint64_t next_frame_ms;
    char code[32];              /* asset code, for the dev overlay */
} layer;

/*
 * Picture planes: bg (%s, %black, bg.*), event (%event, event.*) and the ten bg02 layers.
 * Each holds two pages like the original's page list (+0x14): open replaces the current
 * page, overlap moves it to the back page and fades it out on top of the new one
 * (FUN_0040ade0). Fades step alpha once per frame (FUN_0043e0f0): fadein / fadeout by 5
 * (51 frames), the quick forms by 60, overlapTimeStep by n; a page faded to 0 is dropped.
 * A page can also be a solid colour (bg.black / bg.white, FUN_0040ac40).
 */
#define BG02_LAYERS 10
#define FADE_STEP 5             /* FUN_0043dfd0 / FUN_0043dff0 */
#define FADE_STEP_QUICK 60      /* FUN_0043e010 / FUN_0043e030 */

typedef struct page {
    layer pic;
    bool solid;
    SDL_Color color;
    int alpha;                  /* +0x154 */
    int fade;                   /* +0x150: 0 none, 1 in, 2 out */
    int step;                   /* +0x158 */
} page;

typedef struct plane {
    page cur, back;             /* back draws on top: the outgoing picture of an overlap */
} plane;

typedef struct channel {
    SDL_AudioStream *stream;
    ft_blob blob;
    const uint8_t *pcm;
    uint32_t pcm_size;
    bool loop;
    float gain;                 /* the script's (music.volume) */
    int group;                  /* FT_VOL_*: which of the player's volumes applies */
    char code[32];              /* what's playing, for the dev overlay */
} channel;

/*
 * Audio channels, as the original routes them: %sound.play / playrepeat / stop all use
 * player 1 (kt_cmd_sound), so a new sound replaces the previous one there; voice.*, se1.*,
 * se2.* and music / %midi (player 4) have their own.
 */
enum { CH_SOUND, CH_VOICE, CH_SE1, CH_SE2, CH_MUSIC, CH_COUNT };

/*
 * The film layer, drawn over the scene CG (so %s sets the background behind a film). A
 * film is a set of clips (load = clip 0, addload appends; FUN_00405b90 / FUN_00405cc0),
 * each a ZDF drawn at its header x/y and animated together. Flags mirror the original
 * player: hidden = +0xa0 (OFF/ON), playing = +0x98 (SpeedPlay), thread = +0x9c
 * (SpeedThreadPicON / SpeedPlayLoop).
 *
 * SpeedPlay and SpeedPlayLoop hold the script (FUN_00419240 waits in FUN_00424b60) until the
 * frame update (FUN_00424bd0 / FUN_004053c0) reports done: SpeedPlay when the first clip
 * has played through, SpeedPlayLoop n after n passes, either one when a
 * %timer.loopcut.wait target fires. %film.new resets the rate to 4 (FUN_00405630), and
 * a loaded clip takes the current rate (FUN_00405b90).
 */
#define FILM_RATE_NEW 4
typedef struct film_clip {
    ft_blob blob;
    ft_zdf zdf;
    bool open, done;
    SDL_Texture *tex;
    float w, h;
    float scale;                /* ft_assets_scale: event clips (740 units) x 1024/740 */
    int loops;                  /* extra plays after the first; -1 = forever */
    char code[32];
} film_clip;

typedef struct film {
    film_clip clip[FILM_PARTS];
    int count;
    bool hidden, playing, thread;
    bool blocking;              /* SpeedPlay / SpeedPlayLoop: the script waits */
    int wait_passes;            /* SpeedPlayLoop n: release after n passes; 0 = when clip 0 ends */
    int passes;                 /* +0x60: passes of clip 0 since the play started */
    int rate;                   /* n: n/14 frames per tick */
    float acc;
    uint64_t last_ms;
} film;

/*
 * %sprite (kt_cmd_sprite 0x426410): every load adds a picture, every draw01.setCell adds a
 * cell, and cell k moves picture k. The cell class (0x435b20..0x435f80) keeps positions in
 * tenths of a pixel: a loop starts each cell at x,y x 10 (0x435d70), then every step adds
 * dx,dy (0x435e10) and draws at position x 0.1. Script coordinates are in 740-wide event
 * space and scaled to the screen on the way in (x 1024/740, FUN_00432dd0; setCellx1894 uses
 * x 1024/1894, FUN_00432e00).
 */
#define SPRITE_MAX 48
#define SPRITE_EASE_STEPS 28

enum { SPRITE_COPY, SPRITE_AND, SPRITE_PAINT, SPRITE_MASKED };

typedef struct sprite_cell {
    int mode;                   /* +0x34: 1 move, 2 blink, 3 window, 4 wrap, 5 eased */
    bool on;                    /* +0x4c: setCellON / OFF */
    bool done;                  /* +0x30: ran past count */
    float x, y, dx, dy;         /* [0..3], screen pixels; dx, dy per step in tenths */
    float ex, ey;               /* [4], [5]: mode 5 total offset */
    int count, step;            /* [6], [7] */
    float px, py;               /* +0x28 / +0x2c, tenths of a pixel */
    int frame, frame_start, frame_on, frame_off, frame_len; /* +0x38..+0x48 */
    bool stepdown;              /* +0x50: dx, dy shrink by sdx, sdy until they cross zero */
    float sdx, sdy;
    bool shown;                 /* what the sprite target last drew: visible, at sx, sy (tenths) */
    float sx, sy;
} sprite_cell;

typedef struct sprite_pic {
    SDL_Texture *tex;
    float w, h;
    char code[32];
} sprite_pic;

/*
 * %selSpeed (kt_cmd_selspeed 0x4258a0, player object 0x4358a0): pictures registered with
 * Create, shown at native size and offset by a pan (Scroll) or a shake (Bure). The player
 * runs once per frame (FUN_00435500 from the frame update 0x442ba0, see FRAME_MS) and steps
 * every `period` frames; Bure beats Scroll beats Loop beats Draw. Scroll.Set / SetE100mode
 * only fill a parameter block that %selSpeed.Scroll copies into the running pan
 * (FUN_004256c0), so a pan in flight isn't disturbed by setting up the next one.
 */
#define SEL_PICS 16

typedef struct selspeed {
    sprite_pic pic[SEL_PICS];
    int count;

    /* parameter block (command +0x158..) */
    int number, timer;          /* Scroll.Number, Scroll.Timer / ThreadScrollTimer */
    float set[6];               /* view w, h, dx, dy, start x, y (screen pixels) */
    bool set_e100;
    int set_e100_step;

    /* pan (+0x5c..+0xec) */
    bool scroll, scroll_done;
    int scroll_pic, scroll_period, scroll_limit, scroll_steps;
    float view_w, view_h, dx, dy, x0, y0, x, y;
    bool e100;                  /* +0x9c: eased by a quarter sine (table 0x4f0f60) */
    int e100_idx, e100_step;

    /* shake (+0x00..+0x34, FUN_00418330 / FUN_00418370) */
    bool bure, bure_done;
    int bure_pic, bure_period;
    int amp0_x, amp0_y, amp_x, amp_y, off_x, off_y, phase, bure_count, bure_left, bure_mode;

    /* picture cycling (Loop, +0x110..) and still picture (Draw, +0x108) */
    bool loop;
    int loop_left, loop_pic, loop_period;
    bool draw;
    int draw_pic;

    int frame_acc;              /* frames since the current period started, per player */
    int bure_acc, loop_acc;
} selspeed;

/*
 * Dev overlay (` key, --dev only): an info page with what's on screen and the last lines run, a scene
 * picker (scripts and their labels) that jumps anywhere, and C to copy a bug report to the
 * clipboard. Our addition; the original has nothing like it.
 */
#define DBG_RECENT 12
#define DBG_CACHE 96
#define DBG_FONT_PX 16
#define DBG_ROW 19

typedef struct dbg_entry {
    char s[160];
    SDL_Color color;
    SDL_Texture *tex;
    float w, h;
    uint64_t used;
} dbg_entry;

typedef struct debug_ui {
    int mode;                   /* 0 closed, 1 info, 2 scene picker */
    int col;                    /* picker: 0 scripts, 1 labels */
    int script, label;          /* picker selection (index into order / label list) */
    int top[2];                 /* picker scroll */
    uint32_t *order;            /* script indices sorted by name */
    uint32_t order_count;
    char recent[DBG_RECENT][128];
    int recent_pos;
    uint32_t line_no;           /* source line of the last command / dialogue */
    char dialogue[64];          /* last $id */
    char status[96];            /* one-line feedback (copied, jumped, ...) */
    dbg_entry cache[DBG_CACHE];
    uint64_t tick;
} debug_ui;

struct ft_runtime {
    SDL_Renderer *r;
    ft_assets assets;
    ft_interp in;
    bool finished;

    plane bg, event, bg02[BG02_LAYERS];
    int bg02_current;           /* bg02.current (0x505bd8) */
    SDL_Color backdrop;         /* dd.bg.color / the logo's fade (clear colour 0x4f1418) */
    /*
     * Maker logos (FSystem 8, object at frame +0xe38, FUN_0040c600): bg02lgga, bg02lgcf,
     * bg02lggx at (152, 84). Each fades in, holds 160 frames, then the next replaces it with the
     * old one fading out by 10; after the last, it fades out and the clear colour fades from
     * white to black by 10 per frame. FSystem 9 hides it.
     */
    plane logo;
    int logo_state;             /* +0xc0: 0 off, 1 showing, 2 fading out */
    int logo_next, logo_count, logo_hold, logo_white;
    uint64_t last_frame;        /* frames of FRAME_MS: the render loop's pace */
    layer face[FACE_SLOTS];
    int face_formation;

    SDL_Texture *text;
    float text_w, text_h, text_scale;   /* x TEXT_SCALE for textbmp lines, x1 for font fallback */
    bool text_visible;
    uint64_t hold_until_ms;     /* dialogue hold after a line; a click ends it */
    int hold_ticks;             /* 0x501330: the last hold kt_dialogue_parse set */

    ft_choicemenu *menu;        /* the command menu and the story's choices */
    int menu_pick;
    ft_titlemenu *title;        /* starmenu's menus get our title menu instead (see is_title_menu) */
    int title_pick;             /* the script's entry index, or -1 */
    int title_map[FT_TITLEMENU_ITEMS];  /* shown entry -> script entry (アルバム is left out) */

    /* script clock: value = origin + (now - base) / 10 ms */
    uint64_t clock_base_ms;
    int32_t clock_origin;
    int32_t clock_last_target;  /* %timer.delta adds to this */
    bool clock_waiting;         /* blocked until the clock reaches clock_wait_target */
    int32_t clock_wait_target;
    uint64_t wait_until_ms;     /* plain waits: %timer N, sprite loops */
    char onkey_label[64];       /* OnKeyJump target, "" when not armed */

    SDL_AudioDeviceID audio;    /* opened once: reopening per sound delayed every clip */
    channel ch[CH_COUNT];
    char unhandled[64][48];
    int unhandled_count;

    bool autoplay;
    bool dev;                   /* --dev: the dev overlay and Ctrl fast-forward (ft_runtime_set_dev) */
    uint32_t dialogue_count;

    /* %sprite: see sprite_cell */
    sprite_pic sprite_pic[SPRITE_MAX];
    sprite_cell sprite_cell[SPRITE_MAX];
    int sprite_pics, sprite_cells;
    bool sprite_drawing;        /* +0x40: set by a loop / draw00, cleared by DeleteDDB */
    bool sprite_running;        /* a loop is stepping the cells */
    bool sprite_blocking;       /* draw01.loop / ContinueLoop hold the script; ThreadLoop doesn't */
    int32_t sprite_steps_left;  /* +0x28 */
    float sprite_acc;           /* +0x30: += Timer / 14 per frame, a step at 1 */
    int sprite_timer;           /* %sprite.Timer n (0..100) */
    int32_t sprite_loopcount;   /* %sprite.draw01.loopcount */

    selspeed sel;

    uint32_t film_frame_ms;     /* ZDF CG animation on the scene layer */
    film film;

    bool loopcut_active;        /* %timer.loopcut.wait (0x5c2054): fires once at this clock */
    int32_t loopcut_target;     /* 0x5c2050 */

    /* Display commands go through the render thread, DISPLAY_PER_FRAME per frame (pay_costs). */
    bool cmd_frame_wait;        /* blocked until last_frame reaches cmd_frame_target */
    uint64_t cmd_frame_target;
    uint64_t cmd_frame;         /* frame of the last display command */
    int cmd_count;              /* display commands taken in cmd_frame */

    /* save / load (see save_slot / load_slot) */
    ft_savepanel *panel;
    char save_dir[1024];
    char shot_dir[1024];        /* F12 screenshots (FT_SHOT_DIR, else screenshots/ beside save/) */
    bool want_shot;
    SDL_Texture *shot_notice;   /* "saved" / "failed", shown for a moment (not in the picture) */
    float shot_notice_w, shot_notice_h;
    uint64_t shot_notice_until; /* SDL_GetTicks(): it shows while the game is paused too */
    SDL_Surface *thumb;         /* the scene when the panel opened, for a save's screenshot */
    bool want_thumb;
    /*
     * The last save point the game passed: a command menu, the start of a story script, or a
     * load. Saving anywhere else writes this (see save_slot), so Save is always available.
     */
    struct {
        bool valid;
        ft_save sv;             /* its vars stay empty: they're in `vars` */
        ft_vars vars;
        SDL_Surface *thumb;
        bool want_thumb;        /* take the thumbnail at the next frame */
    } checkpoint;
    int curtain;                /* black over the scene after a load, fading by CURTAIN_STEP a frame */

    /* player settings (see settingspanel.h): stored in cfg_path, applied to every channel */
    ft_config cfg;
    char cfg_path[1024];
    ft_settingspanel *settings;
    bool settings_title;        /* opened in place of system01: back to the title menu after */

    /* the intro movie (start.txt: movieplay.load, movieplay.wait); any key or click skips it */
    ft_movie *movie;
    bool movie_waiting;
    int movie_volume;           /* movieplay.volume, 1000 = full */

    /*
     * The Esc menu (our addition): save, load, settings, quit. While it's open the game is paused:
     * game_ms() stands still and the audio device stops, so waits, fades and voices carry on
     * where they were.
     */
    ft_titlemenu *pause;
    bool pause_quit;            /* showing the quit prompt */
    bool pause_panel;           /* the save panel was opened from it: back returns to it */
    bool paused;
    uint64_t pause_start_ms, paused_ms;

    ft_i18n i18n;               /* translation table (i18n/<lang>.txt), empty if none */
    char lang[32];

    debug_ui dbg;
};

void ft_runtime_set_autoplay(ft_runtime *rt, bool on)
{
    rt->autoplay = on;
}

uint32_t ft_runtime_dialogue_count(const ft_runtime *rt)
{
    return rt->dialogue_count;
}

/* The time the game runs on: game_ms(rt) minus the time spent paused in the Esc menu. */
static uint64_t game_ms(const ft_runtime *rt)
{
    return (rt->paused ? rt->pause_start_ms : SDL_GetTicks()) - rt->paused_ms;
}

static int64_t clock_value(const ft_runtime *rt, uint64_t now);
static int arg_int(const ft_sline *l, int i, ft_interp *in);
static const char *arg(const ft_sline *l, int i);
static void dbg_open_picker(ft_runtime *rt);
static void logo_start(ft_runtime *rt);
static bool pay_costs(ft_runtime *rt, bool display);
static void panel_back(ft_runtime *rt);
static void pause_show(ft_runtime *rt, int sel);
static void pause_close(ft_runtime *rt);
static bool any_save(const ft_runtime *rt);
static void settings_open(ft_runtime *rt, bool from_title);
static void host_entered(void *user, ft_interp *in, const ft_script *s);
static void reset_scene(ft_runtime *rt, uint64_t now);
static void checkpoint_take(ft_runtime *rt);
static bool is_story_script(const char *name);

/* ---- textures ---------------------------------------------------------- */

static SDL_Texture *texture_from(ft_runtime *rt, const ft_image *img)
{
    SDL_Texture *t = SDL_CreateTexture(rt->r, SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_STATIC, (int)img->w, (int)img->h);
    if (!t)
        return NULL;
    SDL_UpdateTexture(t, NULL, img->bgra, (int)img->w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static void layer_clear(layer *l)
{
    if (l->tex)
        SDL_DestroyTexture(l->tex);
    if (l->animated)
        ft_zdf_close(&l->zdf);
    free(l->blob.data);
    memset(l, 0, sizeof *l);
}

/* Shows a pseudo-BMP, or a ZDF animated from its first frame. Takes ownership of `b`. */
static bool layer_set(ft_runtime *rt, layer *l, ft_blob b)
{
    layer_clear(l);
    ft_image img = { 0 };
    bool ok;
    if (ft_zdf_is(b.data, b.size)) {
        ok = ft_zdf_open(&l->zdf, b.data, b.size);
        if (ok && !ft_zdf_next(&l->zdf, &img)) {
            ft_zdf_close(&l->zdf);
            ok = false;
        }
        if (ok) {
            /* animations keep the ZDF (and the entry bytes it reads) until played out */
            l->animated = l->zdf.frame_count > 1;
            if (l->animated)
                l->blob = b;
            else
                ft_zdf_close(&l->zdf);
            l->next_frame_ms = game_ms(rt) + rt->film_frame_ms;
        }
    } else {
        ok = ft_assets_decode(&b, &img);
    }
    if (ok) {
        l->tex = texture_from(rt, &img);
        l->w = (float)img.w;
        l->h = (float)img.h;
    }
    ft_image_free(&img);
    if (!l->animated)
        free(b.data);
    if (!ok)
        memset(l, 0, sizeof *l);
    return ok && l->tex;
}

static void layer_tick(ft_runtime *rt, layer *l, uint64_t now)
{
    if (!l->animated || now < l->next_frame_ms)
        return;
    if (l->zdf.next_frame >= l->zdf.frame_count) { /* played once: hold the last frame */
        l->animated = false;
        ft_zdf_close(&l->zdf);
        free(l->blob.data);
        l->blob.data = NULL;
        return;
    }
    ft_image img;
    if (ft_zdf_next(&l->zdf, &img)) {
        SDL_UpdateTexture(l->tex, NULL, img.bgra, (int)img.w * 4);
        ft_image_free(&img);
    }
    l->next_frame_ms = now + rt->film_frame_ms;
}

/* ---- picture planes (see struct plane) --------------------------------- */

static void page_clear(page *p)
{
    layer_clear(&p->pic);
    memset(p, 0, sizeof *p);
}

static void plane_clear(plane *pl)
{
    page_clear(&pl->cur);
    page_clear(&pl->back);
}

/* Loads `code` into a page at full alpha (FUN_0040acd0); "black" / "white" give a solid page. */
static void page_load(ft_runtime *rt, page *p, const char *code, bool bg02)
{
    page_clear(p);
    p->alpha = 255;
    if (ft_stricmp(code, "black") == 0 || ft_stricmp(code, "white") == 0) {
        uint8_t v = ft_stricmp(code, "white") == 0 ? 255 : 0;
        p->solid = true;
        p->color = (SDL_Color){ v, v, v, 255 };
    } else {
        ft_blob b;
        if (bg02 ? ft_assets_bg02(&rt->assets, code, &b) : ft_assets_cg(&rt->assets, code, &b))
            layer_set(rt, &p->pic, b);
    }
    snprintf(p->pic.code, sizeof p->pic.code, "%s", code);
}

static bool page_shown(const page *p)
{
    return p->solid || p->pic.tex;
}

/* fade: 1 in from alpha 0 (FUN_0043dfd0), 2 out from 255 (FUN_0043dff0); `step` alpha per frame. */
static void page_fade(page *p, int fade, int step)
{
    if (!page_shown(p))
        return;
    p->fade = fade;
    p->step = step;
    p->alpha = fade == 1 ? 0 : 255;
}

static void plane_solid(plane *pl, uint8_t v)
{
    page_clear(&pl->cur);
    pl->cur.solid = true;
    pl->cur.color = (SDL_Color){ v, v, v, 255 };
    pl->cur.alpha = 255;
    snprintf(pl->cur.pic.code, sizeof pl->cur.pic.code, "%s", v ? "white" : "black");
}

/* overlap: the current picture moves to the back page and fades out over the new one. */
static void plane_overlap(ft_runtime *rt, plane *pl, const char *code, bool bg02, int step)
{
    page_clear(&pl->back);
    pl->back = pl->cur;
    memset(&pl->cur, 0, sizeof pl->cur);
    page_load(rt, &pl->cur, code, bg02);
    page_fade(&pl->back, 2, step);
}

/* FUN_0043e0f0, once per frame */
static void page_frame(page *p)
{
    if (p->fade == 1) {
        p->alpha += p->step;
        if (p->alpha >= 255) {
            p->alpha = 255;
            p->fade = 0;
        }
    } else if (p->fade == 2) {
        p->alpha -= p->step;
        if (p->alpha < 1) /* state 3: the page is released */
            page_clear(p);
    }
}

static void plane_frame(plane *pl)
{
    page_frame(&pl->cur);
    page_frame(&pl->back);
}

static void plane_tick(ft_runtime *rt, plane *pl, uint64_t now)
{
    layer_tick(rt, &pl->cur.pic, now);
    layer_tick(rt, &pl->back.pic, now);
}

/* ---- film layer -------------------------------------------------------- */

static void clip_free(film_clip *c)
{
    if (c->open)
        ft_zdf_close(&c->zdf);
    if (c->tex)
        SDL_DestroyTexture(c->tex);
    free(c->blob.data);
    memset(c, 0, sizeof *c);
}

static void film_clear(film *f)
{
    for (int i = 0; i < f->count; i++)
        clip_free(&f->clip[i]);
    f->count = 0;
    f->playing = f->thread = f->hidden = f->blocking = false;
}

/* Shows the clip's current frame. */
static void clip_upload(ft_runtime *rt, film_clip *c)
{
    ft_image img;
    if (!c->open || !ft_zdf_to_image(&c->zdf, &img))
        return;
    if (!c->tex) {
        c->tex = texture_from(rt, &img);
        c->w = (float)img.w;
        c->h = (float)img.h;
    } else {
        SDL_UpdateTexture(c->tex, NULL, img.bgra, (int)img.w * 4);
    }
    ft_image_free(&img);
}

/* Back to the keyframe. */
static void clip_restart(ft_runtime *rt, film_clip *c)
{
    if (c->open)
        ft_zdf_close(&c->zdf);
    c->open = ft_zdf_open(&c->zdf, c->blob.data, c->blob.size) && ft_zdf_next(&c->zdf, NULL);
    c->done = false;
    clip_upload(rt, c);
}

static void film_restart(ft_runtime *rt, film *f, int loops)
{
    f->acc = 0;
    f->last_ms = game_ms(rt);
    for (int i = 0; i < f->count; i++) {
        f->clip[i].loops = loops;
        clip_restart(rt, &f->clip[i]);
    }
}

/* %film.load (slot 0) / %film.addload (a new clip). */
static void film_load(ft_runtime *rt, const char *code, bool append)
{
    film *f = &rt->film;
    ft_blob b;
    if (!ft_assets_film(&rt->assets, code, &b))
        return;
    if (!ft_zdf_is(b.data, b.size)) {
        free(b.data);
        return;
    }
    int slot = append ? f->count : 0;
    if (slot >= FILM_PARTS) {
        free(b.data);
        return;
    }
    if (slot < f->count)
        clip_free(&f->clip[slot]);
    else
        f->count = slot + 1;
    f->clip[slot].blob = b;
    f->clip[slot].scale = ft_assets_scale(b.arc);
    clip_restart(rt, &f->clip[slot]);
    snprintf(f->clip[slot].code, sizeof f->clip[slot].code, "%s", code);
}

static void film_tick(ft_runtime *rt, film *f, uint64_t now)
{
    if (f->count == 0 || !(f->playing || f->thread)) {
        f->last_ms = now;
        return;
    }
    f->acc += (float)(now - f->last_ms) / FILM_TICK_MS * (float)f->rate / 14.0f;
    f->last_ms = now;
    int steps = (int)f->acc;
    f->acc -= (float)steps;
    for (int i = 0; i < f->count && steps > 0; i++) {
        film_clip *c = &f->clip[i];
        bool changed = false;
        for (int s = 0; s < steps && c->open && !c->done; s++) {
            if (c->zdf.next_frame < c->zdf.frame_count) {
                changed = ft_zdf_next(&c->zdf, NULL) || changed;
                continue;
            }
            if (i == 0)
                f->passes++;
            if (c->loops != 0) {
                if (c->loops > 0)
                    c->loops--;
                ft_zdf_close(&c->zdf);
                c->open = ft_zdf_open(&c->zdf, c->blob.data, c->blob.size) && ft_zdf_next(&c->zdf, NULL);
                changed = true;
            } else {
                c->done = true; /* hold the last frame */
            }
        }
        if (changed)
            clip_upload(rt, c);
    }
}

/* FUN_00424bd0 / FUN_004053c0, once per frame: releases a waiting SpeedPlay / SpeedPlayLoop. */
static bool loopcut_fired(ft_runtime *rt, uint64_t now);

static void film_frame(ft_runtime *rt, film *f, uint64_t now)
{
    if (!f->blocking)
        return;
    bool done = f->count == 0 || loopcut_fired(rt, now);
    if (!done)
        done = f->wait_passes > 0 ? f->passes >= f->wait_passes : f->clip[0].done || !f->clip[0].open;
    if (done)
        f->blocking = false;
}

/* ---- sprite layer ------------------------------------------------------ */

static void sprite_clear(ft_runtime *rt)
{
    for (int i = 0; i < rt->sprite_pics; i++)
        if (rt->sprite_pic[i].tex)
            SDL_DestroyTexture(rt->sprite_pic[i].tex);
    memset(rt->sprite_pic, 0, sizeof rt->sprite_pic);
    rt->sprite_pics = rt->sprite_cells = 0;
    rt->sprite_drawing = rt->sprite_running = rt->sprite_blocking = false;
}

/* A CG-tier picture (pseudo-BMP, or the first frame of a ZDF). */
static bool sprite_image(ft_runtime *rt, const char *code, ft_image *img, float *scale)
{
    ft_blob b;
    if (!ft_assets_cg(&rt->assets, code, &b))
        return false;
    if (scale)
        *scale = ft_assets_scale(b.arc);
    bool ok;
    if (ft_zdf_is(b.data, b.size)) {
        ft_zdf z;
        ok = ft_zdf_open(&z, b.data, b.size);
        if (ok) {
            ok = ft_zdf_next(&z, img);
            ft_zdf_close(&z);
        }
    } else {
        ok = ft_assets_decode(&b, img);
    }
    free(b.data);
    return ok;
}

/*
 * loadCOPY: drawn as is, which is opaque except for the alpha pseudo-BMPs: the train frames over
 * the scrolling sky (Ava022 C025AM*, Ava026 C109AM*) and the eyes over C227A1 / C229 (Ava036) are
 * transparent where what's below shows through. loadAND: the mask (black shape on white) multiplies what's below.
 * loadPAINT: the picture (shape on black) is added on top. loadPaintAndMask paint,mask: the
 * mask's darkness becomes the picture's alpha.
 */
static void sprite_load(ft_runtime *rt, int kind, const char *code, const char *mask)
{
    if (rt->sprite_pics >= SPRITE_MAX)
        return;
    sprite_pic *p = &rt->sprite_pic[rt->sprite_pics++]; /* a failed load still takes its index */
    snprintf(p->code, sizeof p->code, "%s", code);
    ft_image img, m;
    float scale = 1.0f;
    if (!sprite_image(rt, code, &img, &scale))
        return;
    for (size_t i = 0; kind != SPRITE_COPY && i < (size_t)img.w * img.h; i++)
        img.bgra[i * 4 + 3] = 0xFF;
    if (kind == SPRITE_MASKED && sprite_image(rt, mask, &m, NULL)) {
        for (uint32_t y = 0; y < img.h && y < m.h; y++)
            for (uint32_t x = 0; x < img.w && x < m.w; x++) {
                const uint8_t *s = m.bgra + ((size_t)y * m.w + x) * 4;
                img.bgra[((size_t)y * img.w + x) * 4 + 3] = (uint8_t)(255 - ((s[0] * 29 + s[1] * 150 + s[2] * 77) >> 8));
            }
        ft_image_free(&m);
    }
    p->tex = texture_from(rt, &img);
    p->w = (float)img.w * scale; /* event-size art is drawn x 1024/740 (FUN_0043f900) */
    p->h = (float)img.h * scale;
    ft_image_free(&img);
    if (p->tex) {
        static const SDL_BlendMode blend[] = { SDL_BLENDMODE_BLEND, SDL_BLENDMODE_MOD, SDL_BLENDMODE_ADD, SDL_BLENDMODE_BLEND };
        SDL_SetTextureBlendMode(p->tex, blend[kind]);
    }
}

/* FUN_00432dd0 / FUN_00432e00: script units to screen pixels, rounded half away from zero. */
static float sprite_px(int v, float scale)
{
    return (float)(int)((float)v * scale + (v < 0 ? -0.5f : 0.5f));
}

#define SPRITE_SCALE (1024.0f / 740.0f)
#define SPRITE_SCALE_1894 (1024.0f / 1894.0f)

/* 0x435d70: a loop (not ContinueLoop) puts every cell back at its start. */
static void cell_start(sprite_cell *c)
{
    c->px = c->x * 10;
    c->py = c->y * 10;
    if (c->mode == 2)
        c->frame = c->frame_start;
    c->step = 0;
    c->done = false;
}

/* 0x435e10: one step. */
static void cell_step(sprite_cell *c)
{
    static const int ease[SPRITE_EASE_STEPS] = { 1, 2, 4, 6, 8, 10, 12, 13, 13, 14, 14, 15, 15, 15,
                                                 15, 15, 15, 14, 14, 13, 13, 12, 10, 8, 6, 4, 2, 1 };
    if (c->done)
        return;
    c->step++;
    if (c->mode == 5) { /* 0x435ce0: eased by the table at 0x4f1188, exact at the last step */
        if (c->count <= 0)
            return;
        if (c->step == c->count) {
            c->px = (c->ex + c->x) * 10;
            c->py = (c->ey + c->y) * 10;
        } else if (c->step < c->count && c->step < SPRITE_EASE_STEPS) {
            float k = (float)ease[c->step] / (float)c->count;
            c->px += k * c->ex;
            c->py += k * c->ey;
        }
        return;
    }
    if (c->mode == 4 && c->step >= c->count) {
        c->step = 0;
        c->px = c->py = 0; /* the original wraps to 0, not to x,y */
    } else if (c->mode != 4 && c->step > c->count) {
        c->done = true;
        return;
    } else {
        c->px += c->dx;
        c->py += c->dy;
    }
    if ((c->mode == 2 || c->mode == 3) && ++c->frame >= c->frame_len)
        c->frame = 0;
    if (c->stepdown) {
        float ox = c->dx, oy = c->dy;
        c->dx -= c->sdx;
        c->dy -= c->sdy;
        if ((c->dx < 0) != (ox < 0) || (c->dy < 0) != (oy < 0)) {
            c->stepdown = false;
            c->dx = c->dy = c->sdx = c->sdy = 0;
        }
    }
}

/* 0x435dd0 */
static bool cell_visible(const sprite_cell *c)
{
    if (!c->on)
        return false;
    if (c->mode == 2)
        return c->frame == 0;
    if (c->mode == 3)
        return c->frame < c->frame_off && c->frame >= c->frame_on;
    return true;
}

/* FUN_00436810: the cell is drawn into the sprite target as it is now. */
static void cell_draw(sprite_cell *c)
{
    c->shown = cell_visible(c);
    c->sx = c->px;
    c->sy = c->py;
}

/*
 * One tick of FUN_00426020: each cell is drawn where it is, then stepped. The screen shows
 * the target, so it trails the cells by a step: a loop of N steps shows N - 1 moves, and the
 * last one appears with the next loop's first draw (Asuka's slide-in at ava021b ends flush
 * with the CG bottom only this way). Cells switched on between loops wait for that draw too.
 */
static void sprite_step_all(ft_runtime *rt)
{
    for (int i = 0; i < rt->sprite_cells; i++) {
        cell_draw(&rt->sprite_cell[i]);
        cell_step(&rt->sprite_cell[i]);
    }
}

/* draw01.loop / ThreadLoop (restart = true) and ContinueLoop. */
static bool sprite_loop(ft_runtime *rt, bool restart, bool blocking, uint64_t now)
{
    for (int i = 0; i < rt->sprite_cells; i++) {
        sprite_cell *c = &rt->sprite_cell[i];
        if (restart)
            cell_start(c);
        else if (!c->done && c->mode == 2) /* 0x435db0 */
            c->frame = c->frame_start;
    }
    rt->sprite_drawing = true;
    rt->sprite_steps_left = rt->sprite_loopcount > 0 ? rt->sprite_loopcount : 0;
    rt->sprite_acc = 0;
    rt->sprite_running = true;
    rt->sprite_blocking = blocking;
    if (SDL_getenv("FT_SPRITE_TRACE"))
        SDL_Log("[sprite %llu ms] loop: %d pictures, %d cells, %d steps at Timer %d%s", (unsigned long long)now,
                rt->sprite_pics, rt->sprite_cells, rt->sprite_steps_left, rt->sprite_timer,
                rt->sprite_blocking ? ", blocking" : "");
    return rt->sprite_blocking;
}

/* FUN_00456b30: the %timer.loopcut.wait target, reported once when the clock reaches it. */
static bool loopcut_fired(ft_runtime *rt, uint64_t now)
{
    if (!rt->loopcut_active || clock_value(rt, now) < rt->loopcut_target)
        return false;
    rt->loopcut_active = false;
    return true;
}

/*
 * FUN_00426020, once per frame: the accumulator gains Timer / 14 and each time it passes 1
 * the cells take a step, so a step lasts 14 / n frames (196 / n ms). The step after the
 * last counted one ends the loop and releases the script; a loopcut zeroes the count.
 */
static void sprite_frame(ft_runtime *rt, uint64_t now)
{
    if (!rt->sprite_running)
        return;
    if (loopcut_fired(rt, now))
        rt->sprite_steps_left = 0;
    rt->sprite_acc += (float)rt->sprite_timer / 14.0f;
    if (rt->sprite_acc < 1.0f)
        return;
    rt->sprite_acc -= 1.0f;
    if (rt->sprite_steps_left < 1) {
        rt->sprite_running = rt->sprite_blocking = false;
        return;
    }
    rt->sprite_steps_left--;
    sprite_step_all(rt);
}

/* Skipping: the loop plays out at once. */
static void sprite_finish(ft_runtime *rt)
{
    if (!rt->sprite_blocking)
        return;
    for (int n = 0; n < rt->sprite_steps_left && n < 10000; n++)
        sprite_step_all(rt);
    rt->sprite_steps_left = 0;
    rt->sprite_running = rt->sprite_blocking = false;
}

/* The cell a setCell* line configures: a new one, or cell `id` for the *Rec forms. */
static sprite_cell *sprite_cell_for(ft_runtime *rt, bool rec, int id)
{
    if (!rec) {
        if (rt->sprite_cells >= SPRITE_MAX)
            return NULL;
        sprite_cell *c = &rt->sprite_cell[rt->sprite_cells++];
        memset(c, 0, sizeof *c); /* 0x435b20 */
        c->mode = 1;
        c->on = true;
        return c;
    }
    return id >= 0 && id < rt->sprite_cells ? &rt->sprite_cell[id] : NULL;
}

/* FUN_00444ce0: ON / OFF / stepdown / copyPosition look cells up by index. */
static sprite_cell *sprite_cell_at(ft_runtime *rt, int id)
{
    return id >= 0 && id < rt->sprite_cells ? &rt->sprite_cell[id] : NULL;
}

/* %sprite.* (kt_cmd_sprite 0x426410); returns whether the script blocks. */
static bool sprite_command(ft_runtime *rt, ft_interp *in, const ft_sline *l, const char *h, uint64_t now)
{
#define A(i) arg_int(l, (i), in)
#define PX(i) sprite_px(A(i), SPRITE_SCALE)
    const char *c = h + 8;
    if (strcmp(c, "new") == 0 || strcmp(c, "delete") == 0) {
        sprite_clear(rt);
    } else if (strcmp(c, "loadCOPY") == 0 || strcmp(c, "loadCOPY2") == 0) {
        sprite_load(rt, SPRITE_COPY, arg(l, 0), NULL);
    } else if (strcmp(c, "loadAND") == 0) {
        sprite_load(rt, SPRITE_AND, arg(l, 0), NULL);
    } else if (strcmp(c, "loadPAINT") == 0 || strcmp(c, "loadPAINT2") == 0) {
        sprite_load(rt, SPRITE_PAINT, arg(l, 0), NULL);
    } else if (strcmp(c, "loadPaintAndMask") == 0 || strcmp(c, "loadPaintAndMask2") == 0) {
        sprite_load(rt, SPRITE_MASKED, arg(l, 0), arg(l, 1));
    } else if (strcmp(c, "DeleteDDB") == 0) {
        rt->sprite_drawing = false; /* 0x435f80 */
    } else if (strcmp(c, "draw00") == 0) {
        rt->sprite_drawing = true;
    } else if (strcmp(c, "Timer") == 0) {
        int n = A(0);
        rt->sprite_timer = n < 0 ? 0 : n > 100 ? 100 : n;
    } else if (strcmp(c, "draw01.loopcount") == 0) {
        rt->sprite_loopcount = A(0);
    } else if (strcmp(c, "draw01.loop") == 0 || strcmp(c, "draw01.ThreadLoop") == 0) {
        return sprite_loop(rt, true, c[7] == 'l', now);
    } else if (strcmp(c, "draw01.ContinueLoop") == 0 || strcmp(c, "draw01.CoutinueLoop") == 0) {
        return sprite_loop(rt, false, true, now);
    } else if (strcmp(c, "draw01.DrawNow") == 0) {
        rt->sprite_drawing = true;
        for (int i = 0; i < rt->sprite_cells; i++)
            cell_draw(&rt->sprite_cell[i]);
    } else if (strncmp(c, "draw01.setCell", 14) == 0 && strcmp(c + 14, "ON") != 0 && strcmp(c + 14, "OFF") != 0) {
        const char *v = c + 14;
        bool rec = strstr(v, "Rec") != NULL;
        int b = rec ? 1 : 0; /* first coordinate argument */
        sprite_cell *cell = sprite_cell_for(rt, rec, rec ? A(0) : 0);
        if (!cell || strcmp(v, "6Rec") == 0) /* 6Rec (FUN_00437c90, 2 uses): not traced */
            return false;
        float scale = strcmp(v, "x1894") == 0 ? SPRITE_SCALE_1894 : SPRITE_SCALE;
        cell->x = sprite_px(A(b), scale);
        cell->y = sprite_px(A(b + 1), scale);
        if (v[0] == '5') { /* 0x435ca0 */
            cell->mode = 5;
            cell->ex = PX(b + 2);
            cell->ey = PX(b + 3);
            cell->count = SPRITE_EASE_STEPS;
            cell->on = true;
            return false;
        }
        cell->dx = sprite_px(A(b + 2), scale);
        cell->dy = sprite_px(A(b + 3), scale);
        if (v[0] == '2') { /* 0x435bc0 */
            cell->mode = 2;
            cell->frame = cell->frame_start = A(b + 4);
            cell->frame_len = A(b + 5);
            cell->count = A(b + 6);
            cell->on = true;
        } else if (v[0] == '3') { /* 0x435c10 */
            cell->mode = 3;
            cell->frame = cell->frame_start = A(b + 4);
            cell->frame_on = A(b + 5);
            cell->frame_off = A(b + 6);
            cell->frame_len = A(b + 7);
            cell->count = A(b + 8);
        } else { /* setCell / x1894 / Rec: 0x435b90; setCell4: 0x435c60 */
            cell->mode = v[0] == '4' ? 4 : 1;
            cell->count = A(b + 4);
            cell->on = true;
        }
    } else if (strcmp(c, "draw01.setCellON") == 0 || strcmp(c, "draw01.setCellOFF") == 0) {
        sprite_cell *cell = sprite_cell_at(rt, A(0));
        if (cell)
            cell->on = c[15] == 'N';
    } else if (strcmp(c, "draw01.setStepDownON") == 0 || strcmp(c, "draw01.setStepDownOFF") == 0) {
        sprite_cell *cell = sprite_cell_at(rt, A(0));
        if (cell) {
            cell->stepdown = c[19] == 'N';
            if (cell->stepdown) {
                cell->sdx = (float)A(1);
                cell->sdy = (float)A(2);
            }
        }
    } else if (strcmp(c, "draw01.copyPosition") == 0) {
        sprite_cell *from = sprite_cell_at(rt, A(0)), *to = sprite_cell_at(rt, A(1));
        if (from && to) {
            to->px = from->px;
            to->py = from->py;
        }
    }
    /* CreateDDB*, ENABLE, DISABLE, SetRenderStateModeBlendOneAll: nothing to do here */
    return false;
#undef PX
#undef A
}

static void sprite_render(ft_runtime *rt)
{
    if (!rt->sprite_drawing)
        return;
    SDL_Rect scene = { 0, 0, SCREEN_W, SCENE_H }; /* cells fly in and out of the CG area */
    SDL_SetRenderClipRect(rt->r, &scene);
    for (int i = 0; i < rt->sprite_pics; i++) {
        const sprite_pic *p = &rt->sprite_pic[i];
        if (!p->tex)
            continue;
        float x = 0, y = 0;
        if (i < rt->sprite_cells) {
            const sprite_cell *c = &rt->sprite_cell[i];
            if (!c->shown)
                continue;
            x = c->sx * 0.1f;
            y = c->sy * 0.1f;
        }
        SDL_FRect dst = { SDL_floorf(x), SDL_floorf(y), p->w, p->h };
        SDL_RenderTexture(rt->r, p->tex, NULL, &dst);
    }
    SDL_SetRenderClipRect(rt->r, NULL);
}

/* ---- %selSpeed --------------------------------------------------------- */

static void sel_clear(selspeed *s)
{
    for (int i = 0; i < s->count; i++)
        if (s->pic[i].tex)
            SDL_DestroyTexture(s->pic[i].tex);
    memset(s, 0, sizeof *s);
    s->bure_period = 4; /* 0x434c20 */
}

static const sprite_pic *sel_pic(const selspeed *s, int i)
{
    return i >= 0 && i < s->count && s->pic[i].tex ? &s->pic[i] : NULL;
}

/* 0x4353d0: a linear pan step; false once it runs into the picture's edge. */
static bool sel_scroll_step(selspeed *s, const sprite_pic *p)
{
    static const int quarter_sine[91] = {
        0,    174,  348,  523,  697,  871,  1045, 1218, 1391, 1564, 1736, 1908, 2079, 2249, 2419, 2588,
        2756, 2923, 3090, 3255, 3420, 3583, 3746, 3907, 4067, 4226, 4383, 4539, 4694, 4848, 4999, 5150,
        5299, 5446, 5591, 5735, 5877, 6018, 6156, 6293, 6427, 6560, 6691, 6819, 6946, 7071, 7193, 7313,
        7431, 7547, 7660, 7771, 7880, 7986, 8090, 8191, 8290, 8386, 8480, 8571, 8660, 8746, 8829, 8910,
        8987, 9063, 9135, 9205, 9271, 9335, 9396, 9455, 9510, 9563, 9612, 9659, 9702, 9743, 9781, 9816,
        9848, 9876, 9902, 9925, 9945, 9961, 9975, 9986, 9993, 9998, 9999,
    };
    float max_x = p->w - s->view_w, max_y = p->h - s->view_h;
    if (s->e100) { /* 0x435240: start + sin(i deg) x d */
        s->e100_idx += s->e100_step;
        int i = s->e100_idx < 0 ? 0 : s->e100_idx > 90 ? 90 : s->e100_idx;
        s->x = (float)quarter_sine[i] * s->dx * 0.0001f + s->x0;
        s->y = (float)quarter_sine[i] * s->dy * 0.0001f + s->y0;
        s->x = s->x > max_x ? max_x : s->x < 0 ? 0 : s->x;
        s->y = s->y > max_y ? max_y : s->y < 0 ? 0 : s->y;
        return s->e100_idx < 90;
    }
    s->x += s->dx;
    s->y += s->dy;
    bool more = true;
    if (s->x > max_x && s->dx > 0) {
        s->x = max_x;
        more = false;
    } else if (s->x < 0 && s->dx < 0) {
        s->x = 0;
        more = false;
    }
    if (s->y > max_y && s->dy > 0) {
        s->y = max_y;
        return false;
    }
    if (s->y < 0 && s->dy < 0) {
        s->y = 0;
        more = false;
    }
    return more;
}

/* 0x418370: one beat of the shake, alternating sides; mode 0 decays over 46 beats. */
static bool sel_bure_step(selspeed *s)
{
    static const int decay[46] = { 9999, 9656, 9325, 9004, 8692, 8390, 8097, 7812, 7535, 7265, 7002, 6745,
                                   6494, 6248, 6008, 5773, 5543, 5317, 5095, 4877, 4663, 4452, 4244, 4040,
                                   3838, 3639, 3443, 3249, 3057, 2867, 2679, 2493, 2308, 2125, 1943, 1763,
                                   1583, 1405, 1227, 1051, 874,  699,  524,  349,  174,  0 };
    int x = s->amp_x, y = s->amp_y;
    if (s->phase == 1) {
        s->phase = 2;
    } else {
        x = -x;
        y = -y;
        s->phase = 1;
    }
    s->off_x = x;
    s->off_y = y;
    if (s->bure_left < 1) {
        s->off_x = s->off_y = 0;
        s->phase = 0;
        return false;
    }
    if (s->bure_mode == 0) {
        int k = s->bure_count - s->bure_left;
        int e = k >= 0 && k < 46 ? decay[k] : 0;
        s->amp_x = e * s->amp0_x / 10000;
        s->amp_y = e * s->amp0_y / 10000;
    }
    if (--s->bure_left == 0 && (s->amp_x || s->amp_y))
        s->off_x = s->off_y = 0;
    if (s->amp_x || s->amp_y)
        return true;
    s->phase = 0;
    return false;
}

/* FUN_00435500, once per frame. */
static void sel_frame(selspeed *s)
{
    if (s->bure) {
        if (++s->bure_acc >= s->bure_period) {
            s->bure_acc = 0;
            if (!s->bure_done && !sel_bure_step(s))
                s->bure_done = true;
        }
    } else if (s->scroll) {
        const sprite_pic *p = sel_pic(s, s->scroll_pic);
        if (p && ++s->frame_acc >= s->scroll_period) {
            s->frame_acc = 0;
            if (!s->scroll_done && !sel_scroll_step(s, p))
                s->scroll_done = true;
            if (s->scroll_limit >= 1 && ++s->scroll_steps >= s->scroll_limit)
                s->scroll_done = true;
        }
    } else if (s->loop) {
        if (s->loop_left < 1) {
            s->loop = false;
            return;
        }
        s->loop_acc += 2; /* +0x128 = 2 ticks per frame against +0x12c = period ticks */
        if (s->loop_acc >= s->loop_period) {
            s->loop_left--;
            s->loop_acc -= s->loop_period;
            if (++s->loop_pic >= s->count)
                s->loop_pic = 0;
        }
    }
}


/* 0x434e30 / 0x418330 */
static void sel_bure_start(selspeed *s)
{
    s->amp_x = s->amp0_x;
    s->amp_y = s->amp0_y;
    s->off_x = s->off_y = 0;
    s->phase = 1;
    if (s->bure_mode == 0)
        s->bure_count = 46;
    s->bure_left = s->bure_count;
    s->bure_acc = 0;
    s->bure = true;
    s->bure_done = false;
}

/* FUN_004256c0 + 0x434da0: the parameter block becomes the running pan. */
static void sel_scroll_start(selspeed *s, int limit)
{
    s->scroll_pic = s->number;
    s->scroll_period = s->timer;
    s->view_w = s->set[0];
    s->view_h = s->set[1];
    s->dx = s->set[2];
    s->dy = s->set[3];
    s->x = s->x0 = s->set[4];
    s->y = s->y0 = s->set[5];
    s->e100 = s->set_e100;
    s->e100_step = s->set_e100_step;
    s->e100_idx = 0;
    s->scroll_limit = limit;
    s->scroll_steps = s->frame_acc = 0;
    s->scroll = true;
    s->scroll_done = false;
}

static bool sel_command(ft_runtime *rt, ft_interp *in, const ft_sline *l, const char *h)
{
#define A(i) arg_int(l, (i), in)
#define PX(i) sprite_px(A(i), SPRITE_SCALE)
    selspeed *s = &rt->sel;
    const char *c = strncmp(h, "%selSpeed.", 10) == 0 ? h + 10 : "Create"; /* %selCreate */
    if (strcmp(c, "Create") == 0) {
        if (s->count < SEL_PICS) {
            sprite_pic *p = &s->pic[s->count++];
            snprintf(p->code, sizeof p->code, "%s", arg(l, 0));
            ft_image img;
            float scale = 1.0f;
            if (sprite_image(rt, arg(l, 0), &img, &scale)) {
                p->tex = texture_from(rt, &img);
                p->w = (float)img.w * scale;
                p->h = (float)img.h * scale;
                ft_image_free(&img);
            }
        }
        s->number = s->timer = 0; /* the block is cleared and a pan in flight stops */
        memset(s->set, 0, sizeof s->set);
        s->set_e100 = false;
        s->set_e100_step = 0;
        s->scroll = s->scroll_done = false;
    } else if (strcmp(c, "DeleteAll") == 0) {
        sel_clear(s);
    } else if (strcmp(c, "Draw") == 0) {
        s->draw_pic = A(0);
        s->draw = true;
    } else if (strcmp(c, "Loop") == 0) {
        s->loop_left = A(0);
        s->loop_pic = s->loop_acc = 0;
        s->loop = true;
    } else if (strcmp(c, "Timer") == 0) {
        int n = A(0);
        s->loop_period = n < 0 ? 0 : n > 60 ? 60 : n;
    } else if (strcmp(c, "Scroll.Set") == 0 || strcmp(c, "Scroll.SetE100mode") == 0) {
        for (int i = 0; i < 6; i++)
            s->set[i] = PX(i);
        if (c[10]) {
            s->set_e100 = true;
            s->set_e100_step = A(6);
        } else {
            s->e100 = false; /* writes the running pan's +0x9c directly */
            s->e100_step = 1;
        }
    } else if (strcmp(c, "Scroll.Number") == 0) {
        s->number = A(0);
    } else if (strcmp(c, "Scroll.Timer") == 0 || strcmp(c, "ThreadScrollTimer") == 0) {
        s->timer = A(0);
    } else if (strcmp(c, "Scroll") == 0 || strcmp(c, "ThreadScrollON") == 0) {
        sel_scroll_start(s, A(0));
    } else if (strcmp(c, "Bure.Set") == 0) {
        s->amp0_x = (int)PX(2); /* 0x4182f0: the first pair isn't used by the shake */
        s->amp0_y = (int)PX(3);
        s->bure_mode = 0;
        s->bure_count = 9999;
    } else if (strcmp(c, "Bure.Number") == 0) {
        s->bure_pic = A(0);
    } else if (strcmp(c, "Bure.Mode") == 0) {
        s->bure_mode = A(0);
    } else if (strcmp(c, "Bure.Count") == 0) {
        s->bure_count = A(0);
    } else if (strcmp(c, "Bure.Timer") == 0) {
        s->bure_period = A(0);
    } else if (strcmp(c, "Bure") == 0 || strcmp(c, "BureThread") == 0 || strcmp(c, "ThreadBureON") == 0) {
        sel_bure_start(s);
    } else if (strcmp(c, "ThreadBureOFF") == 0) {
        s->bure = false;
        s->draw = true;
    }
    /* ThreadScrollSet, ThreadBureSet, ThreadBureTimer, ThreadScrollOFF: nothing in the player */
    return false;
#undef PX
#undef A
}

static void sel_render(ft_runtime *rt)
{
    const selspeed *s = &rt->sel;
    const sprite_pic *p = NULL;
    float x = 0, y = 0;
    if (s->bure) {
        p = sel_pic(s, s->bure_pic);
        x = (float)-s->off_x;
        y = (float)-s->off_y;
    } else if (s->scroll) {
        p = sel_pic(s, s->scroll_pic);
        x = -SDL_floorf(s->x);
        y = -SDL_floorf(s->y);
    } else if (s->loop) {
        p = sel_pic(s, s->loop_pic);
    } else if (s->draw) {
        p = sel_pic(s, s->draw_pic);
    }
    if (!p)
        return;
    SDL_Rect scene = { 0, 0, SCREEN_W, SCENE_H };
    SDL_SetRenderClipRect(rt->r, &scene);
    SDL_FRect dst = { x, y, p->w, p->h };
    SDL_RenderTexture(rt->r, p->tex, NULL, &dst);
    SDL_SetRenderClipRect(rt->r, NULL);
}

static SDL_Texture *text_texture(ft_runtime *rt, const char *utf8, float *w, float *h)
{
    ft_image img;
    if (!utf8[0] || !ft_font_render(utf8, FONT_PX, &img))
        return NULL;
    SDL_Texture *t = texture_from(rt, &img);
    *w = (float)img.w;
    *h = (float)img.h;
    ft_image_free(&img);
    return t;
}

/* textbmp lines are light text on a black background: luminance becomes the white text's alpha. */
static SDL_Texture *textline_texture(ft_runtime *rt, const char *id, float *w, float *h)
{
    ft_blob b;
    ft_image img;
    if (!ft_assets_textline(&rt->assets, id, &b))
        return NULL;
    bool ok = ft_assets_decode(&b, &img);
    free(b.data);
    if (!ok)
        return NULL;
    for (size_t i = 0; i < (size_t)img.w * img.h; i++) {
        uint8_t *p = img.bgra + i * 4;
        int lum = (p[0] * 29 + p[1] * 150 + p[2] * 77) >> 8;
        p[0] = p[1] = p[2] = 255;
        p[3] = (uint8_t)lum;
    }
    SDL_Texture *t = texture_from(rt, &img);
    *w = (float)img.w;
    *h = (float)img.h;
    ft_image_free(&img);
    return t;
}

/* ---- audio ------------------------------------------------------------- */

/* Stops a channel at once; its stream stays bound to the device for the next sound. */
static void channel_stop(channel *c)
{
    if (c->stream)
        SDL_ClearAudioStream(c->stream);
    free(c->blob.data);
    c->blob.data = NULL;
    c->pcm = NULL;
    c->pcm_size = 0;
    c->loop = false;
    c->code[0] = 0;
}

/* The script's gain times the player's volume for the channel's group. */
static void channel_gain(ft_runtime *rt, channel *c)
{
    if (c->stream)
        SDL_SetAudioStreamGain(c->stream, c->gain * ft_config_gain(&rt->cfg, c->group));
}

/* Replaces what the channel plays (takes ownership of `b`); starts without device latency. */
static void channel_play(ft_runtime *rt, channel *c, ft_blob b, bool loop)
{
    if (SDL_getenv("FT_AUDIO_TRACE") && c->stream && c->pcm_size)
        SDL_Log("[audio %llu ms] replacing: %d bytes of %u still queued (%.2f s)", (unsigned long long)game_ms(rt),
                SDL_GetAudioStreamQueued(c->stream), c->pcm_size, SDL_GetAudioStreamQueued(c->stream) / 176400.0);
    channel_stop(c);
    ft_wav wav;
    if (!rt->audio || !ft_wav_parse(b.data, b.size, &wav) || wav.format != 1) {
        free(b.data);
        return;
    }
    SDL_AudioSpec spec = { wav.bits_per_sample == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16, wav.channels, (int)wav.sample_rate };
    if (!c->stream) {
        c->stream = SDL_CreateAudioStream(&spec, NULL);
        if (!c->stream || !SDL_BindAudioStream(rt->audio, c->stream)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "audio stream: %s", SDL_GetError());
            if (c->stream)
                SDL_DestroyAudioStream(c->stream);
            c->stream = NULL;
            free(b.data);
            return;
        }
    } else {
        SDL_SetAudioStreamFormat(c->stream, &spec, NULL);
    }
    c->blob = b;
    c->pcm = wav.data;
    c->pcm_size = wav.data_size;
    c->loop = loop;
    channel_gain(rt, c);
    SDL_PutAudioStreamData(c->stream, c->pcm, (int)c->pcm_size);
}

static void channel_tick(channel *c)
{
    if (c->stream && c->loop && SDL_GetAudioStreamQueued(c->stream) < (int)(c->pcm_size / 4 + 1))
        SDL_PutAudioStreamData(c->stream, c->pcm, (int)c->pcm_size);
}

static void play_sound(ft_runtime *rt, int ch, const char *code, bool loop)
{
    ft_blob b;
    if (SDL_getenv("FT_AUDIO_TRACE"))
        SDL_Log("[audio %llu ms] play %s on channel %d (clock %lld)", (unsigned long long)game_ms(rt), code, ch,
                (long long)clock_value(rt, game_ms(rt)));
    if (ft_assets_sound(&rt->assets, code, &b)) {
        /* %sound.play carries most of the voiced lines, so the archive decides, not the channel */
        rt->ch[ch].group = b.arc == FT_ARC_VOICE ? FT_VOL_VOICE : FT_VOL_SE;
        channel_play(rt, &rt->ch[ch], b, loop);
        snprintf(rt->ch[ch].code, sizeof rt->ch[ch].code, "%s", code);
    }
}

static void play_music(ft_runtime *rt, const char *stem)
{
    ft_blob b;
    if (stem && ft_assets_music(&rt->assets, stem, &b)) {
        channel_play(rt, &rt->ch[CH_MUSIC], b, true);
        snprintf(rt->ch[CH_MUSIC].code, sizeof rt->ch[CH_MUSIC].code, "%s", stem);
    } else {
        channel_stop(&rt->ch[CH_MUSIC]);
    }
}

/* ---- menus ------------------------------------------------------------- */

/* ---- host callbacks ---------------------------------------------------- */

static const ft_script *host_load(void *user, const char *name)
{
    return ft_assets_script(&((ft_runtime *)user)->assets, name);
}

/*
 * After showing a line, mana01 holds the script (kt_script_exec_dialogue -> 0x456c40) for
 * N x 10 timer ticks of 14 ms, N being the number after 」 (kt_dialogue_parse 0x427320;
 * none or 0 -> 100 ticks). A click ends the hold early; the text stays up until replaced.
 * So "」1" (voiced lines, followed by %timer.wait) holds 140 ms and "」20" holds 2.8 s.
 */
#define DIALOGUE_TICK_MS 14

/*
 * The hold, in ticks, the way kt_dialogue_parse (0x427320) reads it: it looks for 」 only
 * in token 1, the first token after "speaker「$id", and reads the number 2 bytes after it
 * (atoi, FUN_00455130); when 」 isn't there the index becomes -1 + 2 = 1. N x 10 ticks, or
 * 100 when that's 0. The tokenizer (char classes at FUN_00437290) ends a token at a space,
 * tab, comma, quote, bracket, ';' or one of ! & < = > | % * + - /, and Shift-JIS bytes are
 * word characters. So a line whose text has ASCII spaces or punctuation before 」 falls back
 * to 1.4 s: Ava011's アスカ「$a11010 Vielen Dank!」1 holds 1.4 s in the VM, which is the
 * pause between "Vielen" (009a) and "Dank" (009b), not 140 ms. With no 「 in token 0 and no
 * 」 in token 1 (narration) it returns before setting the hold, so the previous one stays.
 */
static int dialogue_hold_ticks(const char *text, int previous)
{
    static const char breaks[] = "\t ,\"'()[];!&<=>|%*+-/";
    const char *p = text;
    while (*p && !strchr(breaks, *p)) /* token 0: speaker「$id */
        p++;
    bool open = false;
    for (const char *q = text; q + 3 <= p; q++)
        if (memcmp(q, "\xE3\x80\x8C", 3) == 0) /* 「 */
            open = true;
    while (*p == ' ' || *p == '\t')
        p++;
    const char *tok = p; /* token 1 */
    while (*p && ((uint8_t)*p >= 0x80 || !strchr(breaks, *p)))
        p++;
    size_t len = (size_t)(p - tok);
    long n = 0;
    const char *close = NULL;
    for (const char *q = tok; q + 3 <= tok + len; q++)
        if (memcmp(q, "\xE3\x80\x8D", 3) == 0) { /* 」 */
            close = q;
            break;
        }
    if (!open && !close)
        return previous;
    if (close)
        n = strtol(close + 3, NULL, 10);
    else if (len > 1 && (uint8_t)tok[0] < 0x80) /* index 1: atoi from the token's second byte */
        n = strtol(tok + 1, NULL, 10);
    return n * 10 != 0 ? (int)(n * 10) : 100;
}

/* Dev overlay: remembers the line for the "recent" list and the bug report. */
static void dbg_note(ft_runtime *rt, const ft_sline *l)
{
    debug_ui *d = &rt->dbg;
    d->line_no = l->line_no;
    snprintf(d->recent[d->recent_pos], sizeof d->recent[0], "%s:%u  %s", rt->in.script ? rt->in.script->name : "?",
             l->line_no, l->text);
    d->recent_pos = (d->recent_pos + 1) % DBG_RECENT;
}

static bool host_dialogue(void *user, ft_interp *in, const ft_sline *l)
{
    (void)in;
    ft_runtime *rt = user;
    dbg_note(rt, l);
    snprintf(rt->dbg.dialogue, sizeof rt->dbg.dialogue, "%s", l->argv[0]);
    if (rt->text)
        SDL_DestroyTexture(rt->text);
    const char *tr = l->argv[0][0] ? ft_i18n_get(&rt->i18n, l->argv[0]) : ft_i18n_text(&rt->i18n, l->argv[1]);
    rt->text = NULL;
    if (tr) {
        ft_image img;
        if (ft_font_render_wrapped(tr, TEXT_TR_PX, TEXT_TR_WRAP, &img)) {
            rt->text = texture_from(rt, &img);
            rt->text_w = (float)img.w;
            rt->text_h = (float)img.h;
            ft_image_free(&img);
        }
        rt->text_scale = 1.0f;
    }
    if (!rt->text) {
        rt->text = l->argv[0][0] ? textline_texture(rt, l->argv[0], &rt->text_w, &rt->text_h) : NULL;
        rt->text_scale = TEXT_SCALE;
    }
    if (!rt->text) {
        rt->text = text_texture(rt, l->argv[1], &rt->text_w, &rt->text_h);
        rt->text_scale = 1.0f;
    }
    if (SDL_getenv("FT_AUDIO_TRACE"))
        SDL_Log("[audio %llu ms] line %s (clock %lld)", (unsigned long long)game_ms(rt), l->argv[0],
                (long long)clock_value(rt, game_ms(rt)));
    rt->text_visible = true;
    rt->hold_ticks = dialogue_hold_ticks(l->text, rt->hold_ticks);
    rt->hold_until_ms = game_ms(rt) + (uint64_t)rt->hold_ticks * DIALOGUE_TICK_MS;
    rt->dialogue_count++;
    pay_costs(rt, true); /* kt_script_exec_dialogue posts the line to the render thread too */
    return true;
}

/* starmenu.txt's select02 / selectCancel: the title menu and its quit prompt. */
static bool is_title_menu(const ft_interp *in)
{
    return in->menu == FT_MENU_SELECT_VAR && in->script && ft_stricmp(in->script->name, "starmenu") == 0;
}

static bool any_save(const ft_runtime *rt)
{
    char path[1100];
    for (int i = 1; i <= FT_SAVE_SLOTS; i++) {
        ft_save_path(path, sizeof path, rt->save_dir, i, "sav");
        if (SDL_GetPathInfo(path, NULL))
            return true;
    }
    return false;
}

/*
 * The title menu: アルバム is left out until the album is done (testalbum2 would hang on its
 * unhandled UI), and 続きからはじめる is greyed out while there's nothing to continue.
 */
static int title_choose(ft_runtime *rt, const ft_choice *c, int count)
{
    if (!ft_titlemenu_is_open(rt->title)) {
        const char *texts[FT_TITLEMENU_ITEMS];
        bool enabled[FT_TITLEMENU_ITEMS];
        int n = 0;
        for (int i = 0; i < count && n < FT_TITLEMENU_ITEMS; i++) {
            if (strcmp(c[i].text, "アルバム") == 0)
                continue;
            rt->title_map[n] = i;
            texts[n] = c[i].text;
            enabled[n++] = strcmp(c[i].text, "続きからはじめる") != 0 || any_save(rt);
        }
        ft_titlemenu_open(rt->title, texts, enabled, n, 500, &rt->i18n); /* under the logo */
        rt->title_pick = -1;
        return -1;
    }
    if (rt->title_pick < 0)
        return -1;
    ft_titlemenu_close(rt->title);
    return rt->title_pick;
}

static int host_choose(void *user, ft_interp *in, const ft_choice *c, int count)
{
    ft_runtime *rt = user;
    if (rt->title && is_title_menu(in))
        return title_choose(rt, c, count);
    if (!ft_choicemenu_is_open(rt->menu)) {
        ft_choicemenu_open(rt->menu, in->menu == FT_MENU_CMND ? FT_CHOICE_COMMAND : FT_CHOICE_SELECT, c, count,
                           &rt->i18n);
        rt->menu_pick = -1;
        if (in->menu == FT_MENU_CMND) /* the game's own save point */
            checkpoint_take(rt);
        return -1;
    }
    if (rt->menu_pick < 0)
        return -1;
    ft_choicemenu_close(rt->menu);
    return rt->menu_pick;
}

static int arg_int(const ft_sline *l, int i, ft_interp *in)
{
    return i < l->argc ? ft_expr_eval(&in->vars, l->argv[i], false) : 0;
}

static const char *arg(const ft_sline *l, int i)
{
    return i < l->argc ? l->argv[i] : "";
}

/*
 * bg.* / bg02.* / event.* (kt_cmd_bg 0x41f5e0, kt_cmd_bg02 0x41fc50, kt_cmd_event 0x41ee40),
 * run on the render thread between frames, so none of them blocks the script. The script
 * thread forwards %s as bg.open, %s.clear as bg.close, %event / %eventclose as event.open /
 * event.close, and %black / %qblack / %white / %qwhite as bg.black / bg.white
 * (kt_script_exec_line 0x41b151..0x41b48d). Returns false if `h` isn't one of these.
 */
static bool plane_command(ft_runtime *rt, ft_interp *in, const ft_sline *l, const char *h)
{
    plane *pl;
    const char *c;
    bool bg02 = false, ev = false;
    if (strncmp(h, "bg.", 3) == 0) {
        pl = &rt->bg;
        c = h + 3;
    } else if (strncmp(h, "bg02.", 5) == 0) {
        pl = &rt->bg02[rt->bg02_current];
        c = h + 5;
        bg02 = true;
    } else if (strncmp(h, "event.", 6) == 0) {
        pl = &rt->event;
        c = h + 6;
        ev = true;
    } else {
        return false;
    }
    const char *code = arg(l, 0);
    if (bg02 && strcmp(c, "current") == 0) {
        int n = arg_int(l, 0, in);
        rt->bg02_current = n < 0 ? 0 : n >= BG02_LAYERS ? BG02_LAYERS - 1 : n;
    } else if (strcmp(c, "open") == 0 || strcmp(c, "load") == 0) {
        page_load(rt, &pl->cur, code, bg02);
        if (ev) /* FUN_0040e820: the event layer comes in with a quick fade */
            page_fade(&pl->cur, 1, FADE_STEP_QUICK);
    } else if (strcmp(c, "close") == 0) {
        if (ev) { /* FUN_0040e560: both pages fade out quickly */
            page_fade(&pl->cur, 2, FADE_STEP_QUICK);
            page_fade(&pl->back, 2, FADE_STEP_QUICK);
        } else {
            plane_clear(pl); /* FUN_0040ab80 */
        }
    } else if (strcmp(c, "overlap") == 0) {
        plane_overlap(rt, pl, code, bg02, FADE_STEP);
    } else if (strcmp(c, "overlapQuick") == 0) {
        plane_overlap(rt, pl, code, bg02, FADE_STEP_QUICK);
    } else if (strcmp(c, "overlapTimeStep") == 0) { /* FUN_0040aed0: the old page fades by n */
        int n = arg_int(l, 1, in);
        plane_overlap(rt, pl, code, bg02, n > 0 ? n : 1);
    } else if (strcmp(c, "fadein") == 0) {
        page_load(rt, &pl->cur, code, bg02);
        page_fade(&pl->cur, 1, FADE_STEP);
    } else if (strcmp(c, "fadeout") == 0 || strcmp(c, "fadout") == 0) {
        page_fade(&pl->cur, 2, FADE_STEP);
    } else if (strcmp(c, "black") == 0 || strcmp(c, "white") == 0) {
        plane_solid(pl, c[0] == 'w' ? 255 : 0);
    }
    /* SetUserMatrix, SetRotateZ, SetRenzokuRotateZ, SetResize, SetScrollY, SetPictureAnimeMode:
       picture transforms, not done yet */
    return true;
}

static int64_t clock_value(const ft_runtime *rt, uint64_t now)
{
    return rt->clock_origin + (int64_t)((now - rt->clock_base_ms) / TIMER_UNIT_MS);
}

/* Blocks until the script clock reaches `target`; returns whether it had to wait. */
static bool clock_wait(ft_runtime *rt, int32_t target, uint64_t now)
{
    if (clock_value(rt, now) >= target)
        return false;
    rt->clock_waiting = true;
    rt->clock_wait_target = target;
    return true;
}

static void note_unhandled(ft_runtime *rt, const char *head)
{
    for (int i = 0; i < rt->unhandled_count; i++)
        if (strcmp(rt->unhandled[i], head) == 0)
            return;
    if (rt->unhandled_count < 64) {
        snprintf(rt->unhandled[rt->unhandled_count++], sizeof rt->unhandled[0], "%s", head);
        SDL_Log("unhandled command: %s", head);
    }
}

/* ---- the intro movie (see movie.h) ------------------------------------- */

static void movie_path(const ft_runtime *rt, const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", rt->assets.dir, name);
}

/* movieplay.volume times the player's master and music volumes (the movie is mostly music). */
static float movie_gain(const ft_runtime *rt)
{
    return (float)rt->movie_volume / 1000.0f * ft_config_gain(&rt->cfg, FT_VOL_MUSIC);
}

/* Ends the movie, played out or skipped, and releases a movieplay.wait. */
static void movie_stop(ft_runtime *rt)
{
    ft_movie_close(rt->movie);
    rt->movie = NULL;
    rt->movie_waiting = false;
}

static bool run_command(ft_runtime *rt, ft_interp *in, const ft_sline *l)
{
    const char *h = l->head;
    uint64_t now = game_ms(rt);
    dbg_note(rt, l);

    /* faces: %face slot,name,code  %face2 slot,name,code  %faceX slot,code */
    if (strcmp(h, "%face") == 0 || strcmp(h, "%face2") == 0 || strcmp(h, "%faceX") == 0) {
        int slot = arg_int(l, 0, in);
        const char *code = strcmp(h, "%faceX") == 0 ? arg(l, 1) : arg(l, 2);
        ft_blob b;
        if (slot >= 0 && slot < FACE_SLOTS && ft_assets_face(&rt->assets, code, &b)) {
            layer_set(rt, &rt->face[slot], b);
            snprintf(rt->face[slot].code, sizeof rt->face[slot].code, "%s", code);
        }
    } else if (strcmp(h, "%faceclose") == 0 || strcmp(h, "%facecloseAll") == 0) {
        /* kt_cmd_face 0x427292 / 0x4272c0: both close the face(s), then FUN_00415eb0 takes the
         * dialogue line down too (the VM clears the text exactly at each faceclose) */
        int slot = h[10] ? -1 : arg_int(l, 0, in);
        for (int i = 0; i < FACE_SLOTS; i++)
            if (slot < 0 || i == slot)
                layer_clear(&rt->face[i]);
        rt->text_visible = false;
    } else if (strcmp(h, "%faceFormation") == 0) {
        rt->face_formation = arg_int(l, 0, in);
    }
    /* picture planes: the script thread's forms first, as kt_script_exec_line rewrites them */
    else if (strcmp(h, "%s") == 0) {
        page_load(rt, &rt->bg.cur, arg(l, 0), false);
    } else if (strcmp(h, "%s.clear") == 0) {
        plane_clear(&rt->bg);
    } else if (strcmp(h, "%event") == 0) {
        page_load(rt, &rt->event.cur, arg(l, 0), false);
        page_fade(&rt->event.cur, 1, FADE_STEP_QUICK);
    } else if (strcmp(h, "%eventclose") == 0) {
        page_fade(&rt->event.cur, 2, FADE_STEP_QUICK);
        page_fade(&rt->event.back, 2, FADE_STEP_QUICK);
    } else if (strcmp(h, "%black") == 0 || strcmp(h, "%qblack") == 0) {
        plane_solid(&rt->bg, 0);
    } else if (strcmp(h, "%white") == 0 || strcmp(h, "%qwhite") == 0) {
        plane_solid(&rt->bg, 255);
    } else if (plane_command(rt, in, l, h)) {
    }
    /* script clock (kt_cmd_timer 0x41bbc0) */
    else if (strcmp(h, "%timer.start") == 0) {
        rt->clock_origin = rt->clock_last_target = arg_int(l, 0, in);
        rt->clock_base_ms = now;
    } else if (strcmp(h, "%timer.wait") == 0) {
        rt->clock_last_target = arg_int(l, 0, in);
        return clock_wait(rt, rt->clock_last_target, now);
    } else if (strcmp(h, "%timer.delta") == 0) {
        rt->clock_last_target += arg_int(l, 0, in);
        return clock_wait(rt, rt->clock_last_target, now);
    } else if (strcmp(h, "%timer.adjast") == 0) {
        rt->clock_last_target = arg_int(l, 0, in);
    } else if (strcmp(h, "%timer.loopcut.wait") == 0) {
        /* doesn't block: the next blocking sprite loop runs until the clock reaches N */
        rt->loopcut_active = true;
        rt->loopcut_target = arg_int(l, 0, in);
    } else if (strcmp(h, "%timer.loopcut.wait.off") == 0) {
        rt->loopcut_active = false;
    } else if (strcmp(h, "OnKeyJump") == 0) {
        snprintf(rt->onkey_label, sizeof rt->onkey_label, "%s", arg(l, 0));
    } else if (strcmp(h, "ResetOnKeyJump") == 0) {
        rt->onkey_label[0] = 0;
    } else if (strcmp(h, "%timer") == 0) {
        rt->wait_until_ms = now + (uint64_t)arg_int(l, 0, in) * TIMER_UNIT_MS;
        return true;
    }
    /* audio */
    else if (strcmp(h, "%sound.play") == 0 || strcmp(h, "%sound.playrepeat") == 0) {
        play_sound(rt, CH_SOUND, arg(l, 0), strcmp(h, "%sound.playrepeat") == 0);
    } else if (strcmp(h, "%sound.stop") == 0) {
        channel_stop(&rt->ch[CH_SOUND]);
    } else if (strcmp(h, "voice.play") == 0) {
        play_sound(rt, CH_VOICE, arg(l, 0), false);
    } else if (strcmp(h, "voice.stop") == 0 || strcmp(h, "voice.fadeout") == 0) {
        channel_stop(&rt->ch[CH_VOICE]);
    } else if (strcmp(h, "se1.play") == 0 || strcmp(h, "se2.play") == 0) {
        play_sound(rt, h[2] == '1' ? CH_SE1 : CH_SE2, arg(l, 0), false);
    } else if (strcmp(h, "se1.stop") == 0 || strcmp(h, "se1.fadeout") == 0) {
        channel_stop(&rt->ch[CH_SE1]);
    } else if (strcmp(h, "se2.stop") == 0 || strcmp(h, "se2.fadeout") == 0) {
        channel_stop(&rt->ch[CH_SE2]);
    } else if (strcmp(h, "%midi.play") == 0 || strcmp(h, "%midi.playrepeat") == 0) {
        play_music(rt, ft_midi_track(arg_int(l, 0, in)));
    } else if (strcmp(h, "%midi.stop") == 0 || strcmp(h, "music.stop") == 0 || strcmp(h, "music.fadeout") == 0) {
        channel_stop(&rt->ch[CH_MUSIC]);
    } else if (strcmp(h, "music.play") == 0 || strcmp(h, "music.continue") == 0) {
        play_music(rt, arg(l, 0));
    } else if (strcmp(h, "music.volume") == 0) {
        rt->ch[CH_MUSIC].gain = (float)arg_int(l, 0, in) / 1000.0f;
        channel_gain(rt, &rt->ch[CH_MUSIC]);
    } else if (strcmp(h, "sound.StopAndDefault") == 0) {
        for (int i = 0; i < CH_COUNT; i++)
            channel_stop(&rt->ch[i]);
    }
    else if (strncmp(h, "%sprite.", 8) == 0) {
        return sprite_command(rt, in, l, h, now);
    }
    else if (strncmp(h, "%selSpeed.", 10) == 0 || strcmp(h, "%selCreate") == 0) {
        return sel_command(rt, in, l, h);
    }
    /* %film: the film layer (see struct film) */
    else if (strcmp(h, "%film.new") == 0) {
        film_clear(&rt->film);
        rt->film.rate = FILM_RATE_NEW;
    } else if (strcmp(h, "%film.delete") == 0) {
        film_clear(&rt->film);
    } else if (strcmp(h, "%film.load") == 0 || strcmp(h, "%film.load2") == 0) {
        film_load(rt, arg(l, 0), false);
    } else if (strcmp(h, "%film.addload") == 0 || strcmp(h, "%film.addload2") == 0) {
        film_load(rt, arg(l, 0), true);
    } else if (strcmp(h, "%film.OFF") == 0) {
        rt->film.hidden = true;
    } else if (strcmp(h, "%film.ON") == 0) {
        rt->film.hidden = false;
    } else if (strcmp(h, "%film.Timer") == 0 || strcmp(h, "%film.SpeedThreadTimer") == 0) {
        int n = arg_int(l, 0, in);
        rt->film.rate = n < 0 ? 0 : n > 60 ? 60 : n;
    } else if (strcmp(h, "%film.SpeedPlay") == 0 || strcmp(h, "%film.SpeedThreadPictuer") == 0) {
        int n = arg_int(l, 0, in);
        rt->film.playing = true;
        film_restart(rt, &rt->film, n > 0 ? n - 1 : 0);
        if (strcmp(h, "%film.SpeedThreadPictuer") == 0) /* threaded: doesn't wait */
            return false;
        rt->film.blocking = rt->film.count > 0;
        rt->film.wait_passes = 0;
        rt->film.passes = 0;
        return rt->film.blocking;
    } else if (strcmp(h, "%film.SpeedPlayLoop") == 0) {
        int n = arg_int(l, 0, in);
        rt->film.hidden = false;
        rt->film.thread = true;
        film_restart(rt, &rt->film, -1); /* keeps cycling after the script is released */
        rt->film.blocking = rt->film.count > 0 && (n > 0 || rt->loopcut_active);
        rt->film.wait_passes = n > 0 ? n : INT32_MAX;
        rt->film.passes = 0;
        return rt->film.blocking;
    } else if (strcmp(h, "%film.SpeedThreadPicON") == 0) {
        /*
         * FUN_004051f0 rewinds the clips and +0x9c starts them without waiting; they play
         * once and hold the last frame (Ava011's c007a in the VM: 27 frames, then still).
         * Only SpeedPlayLoop sets loop mode (+0x68).
         */
        rt->film.thread = true;
        film_restart(rt, &rt->film, 0);
    } else if (strcmp(h, "%film.SpeedThreadPicRepartON") == 0) {
        /*
         * The looping films of the NERV HQ walkways: the corridors and escalators load a film
         * and start it with this alone, naming the location's still (GFRONT7B behind GFRONT7A);
         * the V-1 elevator's floor indicator (gfront11, 4 frames) adds it after
         * SpeedThreadPictuer. The clips repeat until %film.delete. Read from the scripts; the
         * original's handler isn't traced.
         */
        if (l->argc > 0)
            page_load(rt, &rt->bg.cur, arg(l, 0), false);
        rt->film.hidden = false;
        rt->film.thread = true;
        film_restart(rt, &rt->film, -1);
    } else if (strcmp(h, "%film.SpeedThreadPicOFF") == 0 || strcmp(h, "%film.SpeedPlayDataDelete") == 0) {
        rt->film.thread = false;
        rt->film.playing = false;
    } else if (strncmp(h, "%film.", 6) == 0) {
        /* SpeedPlayDataMaking, BGEnable, CopyToSel, SpeedPlayEx/PlEx*, SpeedDraw, ...: not done yet */
    }
    /*
     * FSystem n (FUN_00442f10): 1 frees the save / album panels and falls into 3, which frees
     * every bg02 layer (FUN_00442cb0) and bg03 (FUN_0040bee0). The title menu's
     * StartTitleClose ends with FSystem 1, which is what takes the title pictures down.
     * 2 / 7 preload panels, 4 / 5 toggle the window (+0x318c), 8 / 9 show and hide the logo.
     */
    else if (strcmp(h, "FSystem") == 0) {
        int n = arg_int(l, 0, in);
        if (n == 1 || n == 3) {
            for (int i = 0; i < BG02_LAYERS; i++)
                plane_clear(&rt->bg02[i]);
        } else if (n == 8) {
            logo_start(rt);
        } else if (n == 9) { /* FUN_0040c1b0: off at once */
            rt->logo_state = 0;
            plane_clear(&rt->logo);
        }
    }
    /*
     * The save panel (saveload.txt's SaveLoadMain): savepanel.open c1 (0 from a scene's system
     * verb, 1 load-only from the title), then savepanel.select, which waits for our panel. It
     * saves and deletes by itself and loads directly (load_slot), so the script only ever sees
     * 999 (back) and closes it; its confirmation prompts and saveload.* calls go unused.
     */
    else if (strcmp(h, "savepanel.open") == 0) {
        bool can_save = arg_int(l, 0, in) == 0;
        ft_savepanel_open(rt->panel, can_save, &rt->i18n);
        rt->want_thumb = can_save;
    } else if (strcmp(h, "savepanel.select") == 0) {
        if (ft_savepanel_is_open(rt->panel))
            return true;
        panel_back(rt);
    } else if (strcmp(h, "savepanel.close") == 0) {
        ft_savepanel_close(rt->panel);
    } else if (strncmp(h, "saveload.", 9) == 0) {
    }
    /* dd.bg.color r,g,b (FUN_0040c0e0): the colour behind everything (white for the logos) */
    else if (strcmp(h, "dd.bg.color") == 0) {
        rt->backdrop = (SDL_Color){ (Uint8)arg_int(l, 0, in), (Uint8)arg_int(l, 1, in), (Uint8)arg_int(l, 2, in), 255 };
    } else if (strcmp(h, "dd.init") == 0 || strcmp(h, "dd.load") == 0) {
        /* the render thread's text3d script; our render loop is fixed */
    }
    /* UI chrome, title-screen transforms and engine switches with nothing to draw yet */
    else if (strncmp(h, "cover.", 6) == 0 ||
             strcmp(h, "%HoldScreen") == 0 || strcmp(h, "%ColorTrueRGB") == 0 || strncmp(h, "%riz", 4) == 0 ||
             strcmp(h, "framewait") == 0 || strncmp(h, "dbghashi", 8) == 0 ||
             strncmp(h, "bg03.", 5) == 0 ||
             strncmp(h, "select02.", 9) == 0) {
    }
    /* engine queries the scripts branch on (results in c1) */
    else if (strcmp(h, "CheckMovieSkip") == 0) {
        ft_vars_set_int(&in->vars, "c1", 0); /* play it */
    } else if (strcmp(h, "movieplay.CheckFile") == 0) {
        char path[1100];
        movie_path(rt, arg(l, 0), path, sizeof path);
        ft_vars_set_int(&in->vars, "c1", SDL_GetPathInfo(path, NULL) ? 1 : 0);
    } else if (strcmp(h, "movieplay.volume") == 0) {
        rt->movie_volume = arg_int(l, 0, in);
        ft_movie_set_gain(rt->movie, movie_gain(rt));
    } else if (strcmp(h, "movieplay.load") == 0) {
        char path[1100];
        movie_stop(rt);
        movie_path(rt, arg(l, 0), path, sizeof path);
        rt->movie = ft_movie_open(rt->r, rt->audio, path, movie_gain(rt), now);
    } else if (strcmp(h, "movieplay.wait") == 0) {
        rt->movie_waiting = rt->movie != NULL;
        return rt->movie_waiting;
    } else if (strcmp(h, "CheckInitNow") == 0 || strcmp(h, "CheckFullScreenNow") == 0) {
        ft_vars_set_int(&in->vars, "c1", 0);
    } else {
        note_unhandled(rt, h);
    }
    return false;
}

/*
 * Display commands are posted to the render thread and the script waits for them
 * (FUN_00422460 / FUN_00422450 around every %face, %film, %sprite, %selSpeed, bg*, event.*,
 * %s / %event, dd.* and dialogue line). The render thread takes one posted command after
 * each line of its text3d loop (dd.render / dd.wait / loop, kt_script_check_namespace), so a
 * burst of commands runs at a few per frame. Fitted to the VM recording of Ava011's street
 * scene (about 95 sprite commands between sprite loops, 0.71 s longer than without):
 * 2 per frame.
 */
#define DISPLAY_PER_FRAME 2

static bool is_display_command(const char *h)
{
    static const char *const prefixes[] = { "%face", "%film.", "%sprite.", "%selSpeed.", "%selCreate", "bg.",
                                            "bg02.", "bg03.", "event.", "dd.", "FSystem" };
    static const char *const exact[] = { "%s", "%s.clear", "%event", "%eventclose", "%black", "%qblack", "%white",
                                         "%qwhite" };
    for (size_t i = 0; i < SDL_arraysize(prefixes); i++)
        if (strncmp(h, prefixes[i], strlen(prefixes[i])) == 0)
            return true;
    for (size_t i = 0; i < SDL_arraysize(exact); i++)
        if (strcmp(h, exact[i]) == 0)
            return true;
    return false;
}

/* After a display command: once DISPLAY_PER_FRAME have gone this frame, wait for the next. */
static bool pay_costs(ft_runtime *rt, bool display)
{
    if (!display)
        return false;
    if (rt->cmd_frame != rt->last_frame) {
        rt->cmd_frame = rt->last_frame;
        rt->cmd_count = 0;
    }
    if (++rt->cmd_count < DISPLAY_PER_FRAME)
        return false;
    rt->cmd_frame_wait = true;
    rt->cmd_frame_target = rt->last_frame + 1;
    return true;
}

static bool host_command(void *user, ft_interp *in, const ft_sline *l)
{
    ft_runtime *rt = user;
    bool wait = run_command(rt, in, l);
    return pay_costs(rt, is_display_command(l->head)) || wait;
}

/* ---- lifecycle --------------------------------------------------------- */

ft_runtime *ft_runtime_create(SDL_Renderer *renderer, const char *game_dir, const char *script)
{
    ft_runtime *rt = calloc(1, sizeof *rt);
    if (!rt)
        return NULL;
    rt->r = renderer;
    for (int i = 0; i < CH_COUNT; i++) {
        rt->ch[i].gain = 1.0f;
        rt->ch[i].group = i == CH_MUSIC ? FT_VOL_MUSIC : i == CH_VOICE ? FT_VOL_VOICE : FT_VOL_SE;
    }
    rt->sprite_timer = 14; /* assumed: one step per frame until a script sets %sprite.Timer */
    rt->sel.bure_period = 4;
    rt->last_frame = game_ms(rt) / FRAME_MS;
    rt->film_frame_ms = FILM_FRAME_MS;
    rt->movie_volume = 1000;
    rt->film.rate = FILM_RATE_NEW;
    rt->audio = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (!rt->audio)
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "audio device: %s (continuing without sound)", SDL_GetError());
    if (!ft_font_open())
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s (menus and names won't show)", ft_get_error());
    if (!ft_assets_open(&rt->assets, game_dir)) {
        if (rt->audio)
            SDL_CloseAudioDevice(rt->audio);
        ft_font_close();
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "game data: %s", ft_get_error());
        free(rt);
        return NULL;
    }
    /* saves: FT_SAVE_DIR, else the per-user data folder SDL picks for the platform */
    const char *save_dir = SDL_getenv("FT_SAVE_DIR");
    if (save_dir && save_dir[0]) {
        snprintf(rt->save_dir, sizeof rt->save_dir, "%s", save_dir);
    } else {
        char *pref = SDL_GetPrefPath("fantatech", "kotetsu");
        snprintf(rt->save_dir, sizeof rt->save_dir, "%ssave", pref ? pref : "");
        SDL_free(pref);
    }
    rt->panel = ft_savepanel_create(renderer, rt->save_dir);
    const char *shot_dir = SDL_getenv("FT_SHOT_DIR");
    if (shot_dir && shot_dir[0]) {
        snprintf(rt->shot_dir, sizeof rt->shot_dir, "%s", shot_dir);
    } else {
        char *pref = SDL_GetPrefPath("fantatech", "kotetsu");
        snprintf(rt->shot_dir, sizeof rt->shot_dir, "%sscreenshots", pref ? pref : "");
        SDL_free(pref);
    }
    /* settings: FT_CONFIG, else settings.txt in the per-user data folder */
    const char *cfg = SDL_getenv("FT_CONFIG");
    if (cfg && cfg[0]) {
        snprintf(rt->cfg_path, sizeof rt->cfg_path, "%s", cfg);
    } else {
        char *pref = SDL_GetPrefPath("fantatech", "kotetsu");
        snprintf(rt->cfg_path, sizeof rt->cfg_path, "%ssettings.txt", pref ? pref : "");
        SDL_free(pref);
    }
    ft_config_load(&rt->cfg, rt->cfg_path);
    if (rt->cfg.lang[0])
        ft_runtime_set_language(rt, rt->cfg.lang);
    rt->settings = ft_settingspanel_create(renderer);
    rt->title = ft_titlemenu_create(renderer);
    rt->pause = ft_titlemenu_create(renderer);
    rt->menu = ft_choicemenu_create(renderer);
    ft_host host = { rt, host_load, host_dialogue, host_choose, host_command, host_entered };
    ft_interp_init(&rt->in, &host);
    if (!ft_interp_boot(&rt->in, script)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "script: %s", rt->in.error);
        ft_runtime_destroy(rt);
        return NULL;
    }
    return rt;
}

void ft_runtime_set_dev(ft_runtime *rt, bool on)
{
    rt->dev = on;
    rt->dbg.mode = 0;
    const char *dbg = on ? SDL_getenv("FT_DEV_UI") : NULL; /* 1: info page, 2: scene picker, at start */
    if (dbg && dbg[0] == '1')
        rt->dbg.mode = 1;
    else if (dbg && dbg[0] == '2')
        dbg_open_picker(rt);
}

void ft_runtime_destroy(ft_runtime *rt)
{
    if (!rt)
        return;
    for (int i = 0; i < CH_COUNT; i++) {
        channel_stop(&rt->ch[i]);
        if (rt->ch[i].stream)
            SDL_DestroyAudioStream(rt->ch[i].stream);
    }
    if (rt->audio)
        SDL_CloseAudioDevice(rt->audio);
    ft_choicemenu_destroy(rt->menu);
    ft_savepanel_destroy(rt->panel);
    ft_settingspanel_destroy(rt->settings);
    ft_titlemenu_destroy(rt->title);
    ft_titlemenu_destroy(rt->pause);
    if (rt->shot_notice)
        SDL_DestroyTexture(rt->shot_notice);
    SDL_DestroySurface(rt->thumb);
    SDL_DestroySurface(rt->checkpoint.thumb);
    ft_vars_free(&rt->checkpoint.vars);
    film_clear(&rt->film);
    sprite_clear(rt);
    sel_clear(&rt->sel);
    plane_clear(&rt->bg);
    plane_clear(&rt->event);
    for (int i = 0; i < BG02_LAYERS; i++)
        plane_clear(&rt->bg02[i]);
    for (int i = 0; i < FACE_SLOTS; i++)
        layer_clear(&rt->face[i]);
    if (rt->text)
        SDL_DestroyTexture(rt->text);
    for (int i = 0; i < DBG_CACHE; i++)
        if (rt->dbg.cache[i].tex)
            SDL_DestroyTexture(rt->dbg.cache[i].tex);
    free(rt->dbg.order);
    ft_interp_free(&rt->in);
    ft_assets_close(&rt->assets);
    ft_i18n_free(&rt->i18n);
    ft_font_close();
    free(rt);
}

/*
 * Where translation tables live: i18n/ in the working directory, next to the exe (a release, or
 * Contents/Resources in a macOS app) and, for a development build, in the source tree
 * (FT_SOURCE_DIR, set by CMake). Returns the count.
 */
#define I18N_DIRS 3
static int i18n_dirs(char dirs[I18N_DIRS][1024])
{
    int n = 0;
    snprintf(dirs[n++], sizeof dirs[0], "i18n/");
    const char *base = SDL_GetBasePath();
    if (base)
        snprintf(dirs[n++], sizeof dirs[0], "%si18n/", base);
#ifdef FT_SOURCE_DIR
    snprintf(dirs[n++], sizeof dirs[0], "%s/i18n/", FT_SOURCE_DIR);
#endif
    return n;
}

bool ft_runtime_set_language(ft_runtime *rt, const char *lang)
{
    ft_i18n_free(&rt->i18n);
    rt->lang[0] = 0;
    if (!lang || !lang[0]) {
        if (!ft_font_reset())
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", ft_get_error());
        return true;
    }
    char dirs[I18N_DIRS][1024];
    int n = i18n_dirs(dirs);
    for (int i = 0; i < n; i++) {
        char path[1100];
        snprintf(path, sizeof path, "%.1023s%.64s.txt", dirs[i], lang);
        if (!ft_i18n_load(&rt->i18n, path))
            continue;
        snprintf(rt->lang, sizeof rt->lang, "%s", lang);
        SDL_Log("language %s: %u strings from %s", lang, rt->i18n.count, path);
        if (!(rt->i18n.font[0] ? ft_font_open_file(rt->i18n.font) : ft_font_reset()))
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", ft_get_error());
        return true;
    }
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "no translation table i18n/%s.txt", lang);
    ft_font_reset();
    return false;
}

bool ft_runtime_finished(const ft_runtime *rt)
{
    return rt->finished;
}

/* ---- input / update / render ------------------------------------------- */

static void page_finish(page *p)
{
    if (p->fade == 2)
        page_clear(p);
    p->fade = 0;
    p->alpha = 255;
}

static void plane_finish(plane *pl)
{
    page_finish(&pl->cur);
    page_finish(&pl->back);
}

/* Calls `fn` on every picture plane. */
static void planes_each(ft_runtime *rt, void (*fn)(plane *))
{
    fn(&rt->bg);
    fn(&rt->event);
    for (int i = 0; i < BG02_LAYERS; i++)
        fn(&rt->bg02[i]);
    fn(&rt->logo);
}

/* FUN_0040c130 (table 0x4efad4): the next logo; taking the third ends the sequence. */
static const char *logo_take(ft_runtime *rt, bool *all)
{
    static const char *const names[3] = { "bg02lgga", "bg02lgcf", "bg02lggx" };
    int i = rt->logo_next < 0 ? 0 : rt->logo_next;
    rt->logo_next = i + 1;
    if (rt->logo_next > 2) {
        rt->logo_next = 0;
        *all = true;
    }
    return names[i];
}

/* FSystem 8 (FUN_0040c980 -> 0x40c360): the first logo fades in. */
static void logo_start(ft_runtime *rt)
{
    bool all = false;
    rt->logo_next = 0;
    plane_clear(&rt->logo);
    page_load(rt, &rt->logo.cur, logo_take(rt, &all), true);
    page_fade(&rt->logo.cur, 1, FADE_STEP);
    rt->logo_count = 0;
    rt->logo_hold = 160;
    rt->logo_white = 0;
    rt->logo_state = 1;
}

/* FUN_0040c600, once per frame. */
static void logo_frame(ft_runtime *rt)
{
    if (rt->logo_state == 1) {
        if (++rt->logo_count < rt->logo_hold)
            return;
        rt->logo_count = 0;
        if (rt->logo_next != 0) { /* 0x40c2b0: the next logo, the old one fading out by 10 */
            bool all = false;
            plane_overlap(rt, &rt->logo, logo_take(rt, &all), true, 10);
        } else { /* 0x40c400: all three shown */
            page_fade(&rt->logo.cur, 2, FADE_STEP);
            rt->logo_hold = 60;
            rt->logo_white = -255; /* waiting for the fade out, then counts down from 255 */
            rt->logo_state = 2;
        }
    } else if (rt->logo_state == 2) {
        if (rt->logo_white < 0) {
            if (!page_shown(&rt->logo.cur))
                rt->logo_white = 255;
            return;
        }
        rt->logo_white -= 10;
        uint8_t v = (uint8_t)(rt->logo_white > 0 ? rt->logo_white : 0);
        rt->backdrop = (SDL_Color){ v, v, v, 255 };
        if (rt->logo_white < 1) {
            plane_clear(&rt->logo);
            rt->logo_state = 0;
        }
    }
}

/* One frame of the render loop (FRAME_MS): fades, sprite steps, the %selSpeed player. */
static void frame_step(ft_runtime *rt, uint64_t now)
{
    planes_each(rt, plane_frame);
    logo_frame(rt);
    sprite_frame(rt, now);
    film_frame(rt, &rt->film, now);
    sel_frame(&rt->sel);
    if (rt->curtain > 0)
        rt->curtain -= CURTAIN_STEP;
}

/*
 * Ends the current wait the way the original's skip does: the clock jumps to the target.
 * Fades in progress complete too, so fast-forwarding never leaves a half-faded picture.
 */
static void skip_wait(ft_runtime *rt, uint64_t now)
{
    planes_each(rt, plane_finish);
    if (rt->clock_waiting) {
        rt->clock_origin = rt->clock_wait_target;
        rt->clock_base_ms = now;
        rt->clock_waiting = false;
    }
    rt->wait_until_ms = 0;
    rt->hold_until_ms = 0;
    sprite_finish(rt);
    rt->film.blocking = false;
    rt->cmd_frame_wait = false;
}

/*
 * Click / Space / Enter: ends a dialogue hold (menus take their own input). Timed waits are not
 * skippable (voiced scenes stay in sync), except with OnKeyJump armed, which jumps to its
 * label (one-shot, like the original).
 */
static void advance(ft_runtime *rt)
{
    if (rt->onkey_label[0]) {
        char label[64];
        snprintf(label, sizeof label, "%s", rt->onkey_label);
        rt->onkey_label[0] = 0;
        skip_wait(rt, game_ms(rt));
        ft_interp_goto(&rt->in, label);
    } else {
        rt->hold_until_ms = 0;
    }
}

/* ---- settings (see settingspanel.h) ------------------------------------ */

/* The original Japanese (jp.txt) first, then the translations by file name. */
static int cmp_lang(const void *a, const void *b)
{
    const char *x = ((const ft_lang_option *)a)->id, *y = ((const ft_lang_option *)b)->id;
    bool xjp = strcmp(x, "jp") == 0, yjp = strcmp(y, "jp") == 0;
    return xjp != yjp ? (xjp ? -1 : 1) : strcmp(x, y);
}

/* Every i18n/<lang>.txt found, listed by its @name (see cmp_lang for the order). */
static int list_languages(ft_lang_option *out, int cap)
{
    int n = 0;
    char dirs[I18N_DIRS][1024];
    int nd = i18n_dirs(dirs);
    for (int d = 0; d < nd; d++) {
        int count = 0;
        char **files = SDL_GlobDirectory(dirs[d], "*.txt", 0, &count);
        for (int f = 0; files && f < count && n < cap; f++) {
            char id[32];
            snprintf(id, sizeof id, "%.*s", (int)(strlen(files[f]) - 4), files[f]);
            bool dup = false;
            for (int k = 0; k < n; k++)
                dup |= strcmp(out[k].id, id) == 0;
            char path[1100];
            snprintf(path, sizeof path, "%.1023s%.70s", dirs[d], files[f]);
            ft_i18n t;
            if (dup || !ft_i18n_load(&t, path))
                continue;
            snprintf(out[n].id, sizeof out[n].id, "%s", id);
            snprintf(out[n].name, sizeof out[n].name, "%s", t.name[0] ? t.name : id);
            ft_i18n_free(&t);
            n++;
        }
        SDL_free(files);
    }
    qsort(out, (size_t)n, sizeof *out, cmp_lang);
    return n;
}

/* Borderless fullscreen or a window, as the settings say; letterboxing keeps the 1024x768 picture. */
void ft_runtime_apply_display(ft_runtime *rt)
{
    SDL_Window *w = SDL_GetRenderWindow(rt->r);
    if (w && !SDL_SetWindowFullscreen(w, rt->cfg.fullscreen))
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "fullscreen: %s", SDL_GetError());
}

/* F11, from anywhere: flips the setting, applies and saves it. */
static void toggle_fullscreen(ft_runtime *rt)
{
    rt->cfg.fullscreen = !rt->cfg.fullscreen;
    ft_runtime_apply_display(rt);
    ft_settingspanel_set_fullscreen(rt->settings, rt->cfg.fullscreen); /* if it's open */
    if (!ft_config_save(&rt->cfg, rt->cfg_path))
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "settings: %s", ft_get_error());
}

static void settings_open(ft_runtime *rt, bool from_title)
{
    ft_lang_option langs[FT_SETTINGS_LANGS];
    int n = list_languages(langs, FT_SETTINGS_LANGS);
    ft_config cfg = rt->cfg;
    /* what's showing, even if --lang chose it; no table is the original Japanese (jp.txt) */
    snprintf(cfg.lang, sizeof cfg.lang, "%s", rt->lang[0] ? rt->lang : "jp");
    ft_settingspanel_open(rt->settings, &cfg, langs, n, &rt->i18n);
    rt->settings_title = from_title;
}

static void settings_event(ft_runtime *rt, const SDL_Event *ev)
{
    ft_settings_action a = ft_settingspanel_event(rt->settings, ev);
    const ft_config *cfg = ft_settingspanel_config(rt->settings);
    switch (a) {
    case FT_SETTINGS_LANGUAGE:
        rt->cfg = *cfg;
        ft_runtime_set_language(rt, cfg->lang);
        ft_settingspanel_retranslate(rt->settings);
        ft_savepanel_retranslate(rt->panel);
        break;
    case FT_SETTINGS_VOLUME:
        rt->cfg = *cfg;
        for (int i = 0; i < CH_COUNT; i++)
            channel_gain(rt, &rt->ch[i]);
        ft_movie_set_gain(rt->movie, movie_gain(rt));
        break;
    case FT_SETTINGS_DISPLAY:
        rt->cfg = *cfg;
        ft_runtime_apply_display(rt);
        break;
    case FT_SETTINGS_CLOSE:
        rt->cfg = *cfg;
        ft_settingspanel_close(rt->settings);
        if (!ft_config_save(&rt->cfg, rt->cfg_path))
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "settings: %s", ft_get_error());
        if (rt->settings_title) /* system01 would have gone back with "load starmenu" */
            ft_interp_start(&rt->in, "starmenu");
        else if (ft_titlemenu_is_open(rt->pause))
            pause_show(rt, ft_titlemenu_selected(rt->pause)); /* in the language just picked */
        break;
    default:
        break;
    }
}

/*
 * system01 is the original's settings menu (screen resolution, texture and face sizes), which
 * means nothing here: entering it opens ours instead, and closing ours goes back to the title.
 */
static void host_entered(void *user, ft_interp *in, const ft_script *s)
{
    ft_runtime *rt = user;
    if (ft_stricmp(s->name, "system01") == 0)
        settings_open(rt, true);
    else if (ft_stricmp(s->name, "start") == 0 || ft_stricmp(s->name, "starmenu") == 0)
        rt->checkpoint.valid = false; /* back at the title: no game to save */
    else if (in->pc == 0 && is_story_script(s->name)) /* (a load resumes mid-script and sets its own) */
        checkpoint_take(rt);
}

/* ---- save / load (see save.h, savepanel.h) ----------------------------- */

/* Leaves the panel the way SaveLoadMain expects: number 999 is "back". */
static void panel_back(ft_runtime *rt)
{
    ft_savepanel_close(rt->panel);
    ft_vars_set_int(&rt->in.vars, "number", 999);
    ft_vars_set_int(&rt->in.vars, "savedatanum", -1);
}

static bool is_library(const ft_interp *in, const ft_script *s)
{
    for (int i = 0; i < in->library_count; i++)
        if (in->libraries[i] == s)
            return true;
    return false;
}

/*
 * What a save is called: the nearest ☆ save point above where it resumes (the scripts put one
 * under each location, ☆下駄箱 below ○下駄箱), else the nearest ○ location; at the start of a
 * script, the first ○ location below.
 */
static void save_title(const ft_script *s, uint32_t pc, char *out, size_t cap)
{
    out[0] = 0;
    for (uint32_t i = pc < s->count ? pc : s->count; i-- > 0;) {
        const ft_sline *l = &s->lines[i];
        if (l->kind != FT_LINE_LABEL)
            continue;
        if (l->label_kind == FT_LABEL_SAVE) {
            snprintf(out, cap, "%s", l->head);
            return;
        }
        if (l->label_kind == FT_LABEL_LOCATION && !out[0])
            snprintf(out, cap, "%s", l->head);
    }
    for (uint32_t i = pc; !out[0] && i < s->count; i++)
        if (s->lines[i].kind == FT_LINE_LABEL && s->lines[i].label_kind == FT_LABEL_LOCATION)
            snprintf(out, cap, "%s", s->lines[i].head);
}

/* The story: chapters (Ava…), adventure parts (game…) and the endings (ja…), not the title,
 * settings, credits or test scripts. Entering one is a save point. */
static bool is_story_script(const char *name)
{
    return SDL_strncasecmp(name, "ava", 3) == 0 || SDL_strncasecmp(name, "game", 4) == 0 ||
           SDL_strncasecmp(name, "ja", 2) == 0;
}

static void save_page(ft_save *sv, const char *key, const page *p)
{
    if (p->fade != 2 && page_shown(p))
        ft_save_set(sv, key, p->pic.code);
}

static void save_int(ft_save *sv, const char *key, int v)
{
    char s[16];
    snprintf(s, sizeof s, "%d", v);
    ft_save_set(sv, key, s);
}

static int64_t unix_now(void)
{
    SDL_Time now;
    return SDL_GetCurrentTime(&now) ? (int64_t)(now / SDL_NS_PER_SECOND) : 0;
}

/*
 * The game as it is now, as a save (without the variables). Saves are taken where the game
 * offers them: the panel runs inside saveload.txt called from a scene's system verb, or the
 * Esc menu opens while a command menu waits. Dropping saveload's own frames (or stepping over
 * the waiting %cmnd.wait) leaves the scene's return address, just after its %cmnd.wait, so a
 * load re-runs the command menu. The start of a story script works too (pc 0, nothing to step
 * over). Besides the stack and the variables that leaves what's on screen and playing:
 * pictures, faces, music and looping sounds.
 */
static bool capture_save(ft_runtime *rt, ft_save *sv)
{
    ft_interp *in = &rt->in;
    const ft_script *fs[FT_INTERP_STACK + 1];
    uint32_t fpc[FT_INTERP_STACK + 1];
    int n = 0;
    for (int i = 0; i < in->depth; i++, n++) {
        fs[n] = in->stack[i].script;
        fpc[n] = in->stack[i].pc;
    }
    fs[n] = in->script;
    /* at a command menu: after its %cmnd.wait, like the system verb */
    fpc[n++] = in->pc + (in->menu == FT_MENU_CMND && ft_choicemenu_is_open(rt->menu));
    while (n > 0 && is_library(in, fs[n - 1]))
        n--;
    if (n == 0 || n > FT_SAVE_FRAMES) {
        ft_set_error("nothing to save (%d frames)", n);
        return false;
    }

    memset(sv, 0, sizeof *sv);
    sv->time = unix_now();
    for (int i = 0; i < n; i++) {
        snprintf(sv->frame[i].script, sizeof sv->frame[i].script, "%s", fs[i]->name);
        sv->frame[i].line_no = fpc[i] < fs[i]->count ? fs[i]->lines[fpc[i]].line_no : 0;
    }
    sv->frames = n;
    save_title(fs[n - 1], fpc[n - 1], sv->title, sizeof sv->title);

    char key[32];
    save_page(sv, "bg", &rt->bg.cur);
    save_page(sv, "event", &rt->event.cur);
    for (int i = 0; i < BG02_LAYERS; i++) {
        snprintf(key, sizeof key, "bg02.%d", i);
        save_page(sv, key, &rt->bg02[i].cur);
    }
    save_int(sv, "bg02.current", rt->bg02_current);
    for (int i = 0; i < FACE_SLOTS; i++)
        if (rt->face[i].tex) {
            snprintf(key, sizeof key, "face.%d", i);
            ft_save_set(sv, key, rt->face[i].code);
        }
    save_int(sv, "face.formation", rt->face_formation);
    ft_save_set(sv, "music", rt->ch[CH_MUSIC].code);
    save_int(sv, "music.volume", (int)SDL_lroundf(rt->ch[CH_MUSIC].gain * 1000.0f));
    for (int c = 0; c < CH_COUNT; c++)
        if (c != CH_MUSIC && rt->ch[c].loop) {
            snprintf(key, sizeof key, "loop.%d", c);
            ft_save_set(sv, key, rt->ch[c].code);
        }
    return true;
}

/* Can a save be taken from here (see capture_save)? Otherwise it's the last checkpoint. */
static bool at_save_point(const ft_runtime *rt)
{
    if (!rt->pause_panel) /* the panel from a scene's system verb */
        return true;
    return ft_choicemenu_is_open(rt->menu) && rt->in.menu == FT_MENU_CMND;
}

/* Remembers the game as it is now as the last save point. */
static void checkpoint_take(ft_runtime *rt)
{
    rt->checkpoint.valid =
        capture_save(rt, &rt->checkpoint.sv) && ft_vars_copy(&rt->checkpoint.vars, &rt->in.vars);
    rt->checkpoint.want_thumb = rt->checkpoint.valid;
}

/* Writes slot `slot`: the game as it is at a save point, else the last one it passed. */
static bool save_slot(ft_runtime *rt, int slot, bool *from_checkpoint)
{
    static ft_save sv; /* ~6 KB of kv strings */
    const ft_vars *vars;
    SDL_Surface *thumb;
    *from_checkpoint = false;
    if (at_save_point(rt)) {
        if (!capture_save(rt, &sv))
            return false;
        vars = &rt->in.vars;
        thumb = rt->thumb;
    } else if (rt->checkpoint.valid) {
        sv = rt->checkpoint.sv;
        sv.time = unix_now();
        vars = &rt->checkpoint.vars;
        thumb = rt->checkpoint.thumb;
        *from_checkpoint = true;
    } else {
        ft_set_error("no save point reached yet");
        return false;
    }

    char path[1100];
    ft_save_path(path, sizeof path, rt->save_dir, slot, "sav");
    if (!ft_mkdirs(rt->save_dir) || !ft_save_write(path, &sv, vars))
        return false;
    ft_save_path(path, sizeof path, rt->save_dir, slot, "png");
    if (!thumb || !SDL_SavePNG(thumb, path))
        ft_remove(path);
    return true;
}

static int save_get_int(const ft_save *sv, const char *key, int fallback)
{
    const char *v = ft_save_get(sv, key);
    return v ? atoi(v) : fallback;
}

static bool load_slot(ft_runtime *rt, int slot)
{
    char path[1100];
    static ft_save sv;
    ft_save_path(path, sizeof path, rt->save_dir, slot, "sav");
    if (!ft_save_read(path, &sv, true)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "load %s: not a save file", path);
        return false;
    }
    const char *names[FT_SAVE_FRAMES];
    uint32_t lines[FT_SAVE_FRAMES];
    for (int i = 0; i < sv.frames; i++) {
        names[i] = sv.frame[i].script;
        lines[i] = sv.frame[i].line_no;
    }
    if (!ft_interp_resume(&rt->in, names, lines, sv.frames)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "load %s: %s", path, rt->in.error);
        ft_save_free(&sv);
        return false;
    }
    reset_scene(rt, game_ms(rt));
    ft_vars_free(&rt->in.vars);
    rt->in.vars = sv.vars;
    ft_vars_init(&sv.vars);

    const char *v;
    char key[32];
    if ((v = ft_save_get(&sv, "bg")))
        page_load(rt, &rt->bg.cur, v, false);
    if ((v = ft_save_get(&sv, "event")))
        page_load(rt, &rt->event.cur, v, false);
    for (int i = 0; i < BG02_LAYERS; i++) {
        snprintf(key, sizeof key, "bg02.%d", i);
        if ((v = ft_save_get(&sv, key)))
            page_load(rt, &rt->bg02[i].cur, v, true);
    }
    rt->bg02_current = SDL_clamp(save_get_int(&sv, "bg02.current", 0), 0, BG02_LAYERS - 1);
    for (int i = 0; i < FACE_SLOTS; i++) {
        snprintf(key, sizeof key, "face.%d", i);
        ft_blob b;
        if ((v = ft_save_get(&sv, key)) && ft_assets_face(&rt->assets, v, &b) && layer_set(rt, &rt->face[i], b))
            snprintf(rt->face[i].code, sizeof rt->face[i].code, "%s", v);
    }
    rt->face_formation = save_get_int(&sv, "face.formation", rt->face_formation);
    rt->ch[CH_MUSIC].gain = (float)save_get_int(&sv, "music.volume", 1000) / 1000.0f;
    if ((v = ft_save_get(&sv, "music")))
        play_music(rt, v);
    for (int c = 0; c < CH_COUNT; c++) {
        snprintf(key, sizeof key, "loop.%d", c);
        if (c != CH_MUSIC && (v = ft_save_get(&sv, key)))
            play_sound(rt, c, v, true);
    }
    rt->curtain = 255;
    snprintf(rt->dbg.status, sizeof rt->dbg.status, "loaded slot %d (%.60s)", slot, sv.title);
    /* the loaded save is the last save point now (its vars moved into the interpreter above) */
    rt->checkpoint.sv = sv;
    rt->checkpoint.valid = ft_vars_copy(&rt->checkpoint.vars, &rt->in.vars);
    rt->checkpoint.want_thumb = rt->checkpoint.valid;
    ft_save_free(&sv);
    return true;
}

static void panel_event(ft_runtime *rt, const SDL_Event *ev)
{
    int slot;
    char path[1100];
    bool earlier;
    switch (ft_savepanel_event(rt->panel, ev, &slot)) {
    case FT_PANEL_SAVE:
        if (save_slot(rt, slot, &earlier)) {
            ft_savepanel_refresh(rt->panel, slot);
            ft_savepanel_notice(rt->panel, earlier ? "前のセーブポイントでセーブしました" : "セーブしました");
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "save: %s", ft_get_error());
            ft_savepanel_notice(rt->panel, "セーブできませんでした");
        }
        break;
    case FT_PANEL_LOAD:
        if (!load_slot(rt, slot))
            ft_savepanel_notice(rt->panel, "読み込めませんでした");
        else if (rt->paused)
            pause_close(rt);
        break;
    case FT_PANEL_DELETE:
        ft_save_path(path, sizeof path, rt->save_dir, slot, "sav");
        ft_remove(path);
        ft_save_path(path, sizeof path, rt->save_dir, slot, "png");
        ft_remove(path);
        ft_savepanel_refresh(rt->panel, slot);
        ft_savepanel_notice(rt->panel, "削除しました");
        break;
    case FT_PANEL_BACK:
        if (rt->pause_panel) { /* back to the Esc menu; the script isn't in savepanel.select */
            ft_savepanel_close(rt->panel);
            rt->pause_panel = false;
        } else {
            panel_back(rt);
        }
        break;
    case FT_PANEL_SETTINGS:
        settings_open(rt, false);
        break;
    default:
        break;
    }
}

/* ---- Esc menu ---------------------------------------------------------- */

enum { PAUSE_SAVE, PAUSE_LOAD, PAUSE_SETTINGS, PAUSE_QUIT, PAUSE_RESUME, PAUSE_ITEMS };

/* The entries (`sel` highlighted, or the first available), or the quit prompt. Away from a save
 * point, Save writes the last one the game passed (see save_slot). */
static void pause_show(ft_runtime *rt, int sel)
{
    static const char *const items[PAUSE_ITEMS] = { "セーブ", "ロード", "設定", "終了する", "ゲームに戻る" };
    static const char *const quit[2] = { "ゲームを終了する", "メニューに戻る" };
    if (rt->pause_quit) {
        ft_titlemenu_open(rt->pause, quit, NULL, 2, SCREEN_H / 2, &rt->i18n);
        return;
    }
    bool enabled[PAUSE_ITEMS] = { true, true, true, true, true };
    enabled[PAUSE_SAVE] = (ft_choicemenu_is_open(rt->menu) && rt->in.menu == FT_MENU_CMND) || rt->checkpoint.valid;
    enabled[PAUSE_LOAD] = any_save(rt);
    ft_titlemenu_open(rt->pause, items, enabled, PAUSE_ITEMS, SCREEN_H / 2, &rt->i18n);
    ft_titlemenu_select(rt->pause, sel);
}

static void pause_open(ft_runtime *rt)
{
    if (!rt->paused) {
        rt->pause_start_ms = SDL_GetTicks();
        rt->paused = true;
        if (rt->audio)
            SDL_PauseAudioDevice(rt->audio);
    }
    rt->pause_quit = false;
    rt->want_thumb = true; /* the scene as it is, for a save */
    pause_show(rt, -1);
}

static void pause_close(ft_runtime *rt)
{
    ft_titlemenu_close(rt->pause);
    ft_savepanel_close(rt->panel);
    rt->pause_panel = false;
    if (rt->paused) {
        rt->paused_ms += SDL_GetTicks() - rt->pause_start_ms;
        rt->paused = false;
        if (rt->audio)
            SDL_ResumeAudioDevice(rt->audio);
    }
}

static void pause_pick(ft_runtime *rt, int pick)
{
    if (rt->pause_quit) {
        if (pick == 0)
            rt->finished = true;
        rt->pause_quit = false;
        pause_show(rt, PAUSE_QUIT);
        return;
    }
    switch (pick) {
    case PAUSE_SAVE:
    case PAUSE_LOAD:
        ft_savepanel_open(rt->panel, pick == PAUSE_SAVE, &rt->i18n);
        rt->pause_panel = true;
        break;
    case PAUSE_SETTINGS:
        settings_open(rt, false);
        break;
    case PAUSE_QUIT:
        rt->pause_quit = true;
        pause_show(rt, -1);
        break;
    default:
        pause_close(rt);
        break;
    }
}

static void pause_event(ft_runtime *rt, const SDL_Event *ev)
{
    if (ev->type == SDL_EVENT_KEY_DOWN && (ev->key.key == SDLK_ESCAPE || ev->key.key == SDLK_BACKSPACE)) {
        if (rt->pause_quit) {
            rt->pause_quit = false;
            pause_show(rt, PAUSE_QUIT);
        } else {
            pause_close(rt);
        }
        return;
    }
    int pick = ft_titlemenu_event(rt->pause, ev);
    if (pick >= 0)
        pause_pick(rt, pick);
}

/* ---- dev overlay (see debug_ui) ---------------------------------------- */

/* Drops everything on screen and every wait, for a debug jump. */
static void reset_scene(ft_runtime *rt, uint64_t now)
{
    movie_stop(rt);
    for (int i = 0; i < CH_COUNT; i++)
        channel_stop(&rt->ch[i]);
    ft_choicemenu_close(rt->menu);
    ft_savepanel_close(rt->panel);
    ft_titlemenu_close(rt->title);
    rt->curtain = 0;
    film_clear(&rt->film);
    rt->film.rate = FILM_RATE_NEW;
    sprite_clear(rt);
    sel_clear(&rt->sel);
    planes_each(rt, plane_clear);
    rt->bg02_current = 0;
    rt->logo_state = 0;
    rt->backdrop = (SDL_Color){ 0, 0, 0, 255 };
    for (int i = 0; i < FACE_SLOTS; i++)
        layer_clear(&rt->face[i]);
    if (rt->text)
        SDL_DestroyTexture(rt->text);
    rt->text = NULL;
    rt->text_visible = false;
    rt->clock_waiting = false;
    rt->wait_until_ms = rt->hold_until_ms = 0;
    rt->clock_origin = rt->clock_last_target = 0;
    rt->clock_base_ms = now;
    rt->onkey_label[0] = 0;
    rt->loopcut_active = false;
    rt->cmd_frame_wait = false;
    rt->finished = false;
}

static void dbg_jump(ft_runtime *rt, const char *script, uint32_t line)
{
    reset_scene(rt, game_ms(rt));
    if (ft_interp_jump(&rt->in, script, line))
        snprintf(rt->dbg.status, sizeof rt->dbg.status, "jumped to %s line %u", script,
                 line < rt->in.script->count ? rt->in.script->lines[line].line_no : 0);
    else
        snprintf(rt->dbg.status, sizeof rt->dbg.status, "jump failed: %.80s", rt->in.error);
}

bool ft_runtime_jump_label(ft_runtime *rt, const char *label)
{
    if (!rt->in.script)
        return false;
    int32_t t = ft_script_find_label(rt->in.script, label, FT_LABEL_NONE);
    if (t < 0)
        return false;
    dbg_jump(rt, rt->in.script->name, (uint32_t)t);
    return true;
}

static const char *label_glyph(uint8_t kind)
{
    switch (kind) {
    case FT_LABEL_LOCATION: return "○";
    case FT_LABEL_CHOICE: return "＊";
    case FT_LABEL_HASH: return "#";
    case FT_LABEL_SAVE: return "☆";
    default: return "";
    }
}

/* The label at or above line index `pc` in the running script, or NULL. */
static const ft_sline *dbg_current_label(const ft_runtime *rt, uint32_t *index)
{
    const ft_script *s = rt->in.script;
    if (!s || !s->count)
        return NULL;
    for (int64_t i = rt->in.pc < s->count ? rt->in.pc : s->count - 1; i >= 0; i--)
        if (s->lines[i].kind == FT_LINE_LABEL) {
            if (index)
                *index = (uint32_t)i;
            return &s->lines[i];
        }
    return NULL;
}

static size_t dbg_append(char *buf, size_t size, size_t len, const char *fmt, ...)
{
    if (len >= size)
        return len;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + len, size - len, fmt, ap);
    va_end(ap);
    return n < 0 ? len : len + (size_t)n;
}

static size_t dbg_page_line(char *buf, size_t size, size_t len, const char *name, const page *p)
{
    if (!page_shown(p))
        return len;
    static const char *fades[] = { "", " fading in", " fading out" };
    return dbg_append(buf, size, len, " %s=%s(a%d%s)", name, p->pic.code[0] ? p->pic.code : "?", p->alpha,
                      fades[p->fade % 3]);
}

/* The bug report: what's running and what's on screen, one fact per line. */
static void dbg_report(const ft_runtime *rt, char *buf, size_t size)
{
    const debug_ui *d = &rt->dbg;
    uint64_t now = game_ms(rt);
    size_t n = 0;
    const ft_sline *lab = dbg_current_label(rt, NULL);
    n = dbg_append(buf, size, n, "script %s  line %u  label %s%s\n", rt->in.script ? rt->in.script->name : "-",
                   d->line_no, lab ? label_glyph(lab->label_kind) : "", lab ? lab->head : "-");
    n = dbg_append(buf, size, n, "clock %lld", (long long)clock_value(rt, now));
    if (rt->clock_waiting)
        n = dbg_append(buf, size, n, "  waiting for clock %d", rt->clock_wait_target);
    if (now < rt->hold_until_ms)
        n = dbg_append(buf, size, n, "  dialogue hold %llu ms", (unsigned long long)(rt->hold_until_ms - now));
    if (now < rt->wait_until_ms)
        n = dbg_append(buf, size, n, "  %%timer %llu ms", (unsigned long long)(rt->wait_until_ms - now));
    if (rt->sprite_blocking)
        n = dbg_append(buf, size, n, "  sprite loop (%d steps left)", rt->sprite_steps_left);
    if (rt->film.blocking)
        n = dbg_append(buf, size, n, "  film play (%d/%d passes)", rt->film.passes, rt->film.wait_passes);
    if (ft_choicemenu_is_open(rt->menu))
        n = dbg_append(buf, size, n, "  menu");
    n = dbg_append(buf, size, n, "\ndialogue %s", d->dialogue[0] ? d->dialogue : "-");
    if (rt->lang[0])
        n = dbg_append(buf, size, n, "  [%s: %s, %u strings]", rt->lang,
                       ft_i18n_get(&rt->i18n, d->dialogue) ? "translated" : "not translated", rt->i18n.count);
    n = dbg_append(buf, size, n, "\n");
    n = dbg_append(buf, size, n, "planes:");
    n = dbg_page_line(buf, size, n, "bg", &rt->bg.cur);
    n = dbg_page_line(buf, size, n, "bg.back", &rt->bg.back);
    n = dbg_page_line(buf, size, n, "event", &rt->event.cur);
    n = dbg_page_line(buf, size, n, "event.back", &rt->event.back);
    for (int i = 0; i < BG02_LAYERS; i++) {
        char name[16];
        snprintf(name, sizeof name, "bg02[%d]", i);
        n = dbg_page_line(buf, size, n, name, &rt->bg02[i].cur);
    }
    n = dbg_append(buf, size, n, "  (bg02.current %d)\nfaces (formation %d):", rt->bg02_current, rt->face_formation);
    for (int i = 0; i < FACE_SLOTS; i++)
        if (rt->face[i].tex)
            n = dbg_append(buf, size, n, " %d=%s", i, rt->face[i].code);
    n = dbg_append(buf, size, n, "\nfilm: rate %d%s%s%s", rt->film.rate, rt->film.hidden ? " hidden" : "",
                   rt->film.playing ? " playing" : "", rt->film.thread ? " thread" : "");
    for (int i = 0; i < rt->film.count; i++)
        n = dbg_append(buf, size, n, " %s(%u/%u)", rt->film.clip[i].code, rt->film.clip[i].zdf.next_frame,
                       rt->film.clip[i].zdf.frame_count);
    n = dbg_append(buf, size, n, "\nsprite: %d pics, %d cells, Timer %d%s%s", rt->sprite_pics, rt->sprite_cells,
                   rt->sprite_timer, rt->sprite_drawing ? " drawing" : "", rt->sprite_running ? " running" : "");
    for (int i = 0; i < rt->sprite_pics && i < 12; i++)
        n = dbg_append(buf, size, n, " %s", rt->sprite_pic[i].code);
    const selspeed *s = &rt->sel;
    n = dbg_append(buf, size, n, "\nselSpeed: %d pics%s%s%s%s", s->count, s->bure ? " bure" : "", s->scroll ? " scroll" : "",
                   s->loop ? " loop" : "", s->draw ? " draw" : "");
    for (int i = 0; i < s->count; i++)
        n = dbg_append(buf, size, n, " %s", s->pic[i].code);
    static const char *chname[CH_COUNT] = { "sound", "voice", "se1", "se2", "music" };
    n = dbg_append(buf, size, n, "\naudio:");
    for (int i = 0; i < CH_COUNT; i++)
        if (rt->ch[i].code[0])
            n = dbg_append(buf, size, n, " %s=%s", chname[i], rt->ch[i].code);
    if (rt->unhandled_count) {
        n = dbg_append(buf, size, n, "\nunhandled:");
        for (int i = 0; i < rt->unhandled_count; i++)
            n = dbg_append(buf, size, n, " %s", rt->unhandled[i]);
    }
    n = dbg_append(buf, size, n, "\nrecent lines:\n");
    for (int i = 0; i < DBG_RECENT; i++) {
        const char *r = d->recent[(d->recent_pos + i) % DBG_RECENT];
        if (r[0])
            n = dbg_append(buf, size, n, "  %s\n", r);
    }
}

static int dbg_cmp_script(void *ctx, const void *a, const void *b)
{
    const ft_assets *as = ctx;
    return ft_stricmp(as->scripts[*(const uint32_t *)a].name, as->scripts[*(const uint32_t *)b].name);
}

static void dbg_open_picker(ft_runtime *rt)
{
    debug_ui *d = &rt->dbg;
    if (!d->order) {
        d->order = malloc(sizeof *d->order * (rt->assets.script_count ? rt->assets.script_count : 1));
        if (!d->order)
            return;
        for (uint32_t i = 0; i < rt->assets.script_count; i++)
            d->order[i] = i;
        d->order_count = rt->assets.script_count;
        SDL_qsort_r(d->order, d->order_count, sizeof *d->order, dbg_cmp_script, &rt->assets);
    }
    d->mode = 2;
    d->col = 0;
    d->label = 0;
    for (uint32_t i = 0; rt->in.script && i < d->order_count; i++)
        if (strcmp(rt->assets.scripts[d->order[i]].name, rt->in.script->name) == 0)
            d->script = (int)i;
}

static const ft_script *dbg_picked_script(const ft_runtime *rt)
{
    const debug_ui *d = &rt->dbg;
    return d->order && d->script >= 0 && (uint32_t)d->script < d->order_count ? &rt->assets.scripts[d->order[d->script]]
                                                                             : NULL;
}

#define DBG_ROWS ((SCREEN_H - 90) / DBG_ROW)

/* Keys while the overlay is open; returns false to let the game have the key. */
static bool dbg_key(ft_runtime *rt, SDL_Keycode k)
{
    debug_ui *d = &rt->dbg;
    if (k == SDLK_GRAVE) {
        d->mode = d->mode ? 0 : 1;
        d->status[0] = 0;
        return true;
    }
    if (!d->mode)
        return false;
    if (d->mode == 1) {
        if (k == SDLK_S) {
            dbg_open_picker(rt);
        } else if (k == SDLK_C) {
            static char report[8192];
            dbg_report(rt, report, sizeof report);
            snprintf(d->status, sizeof d->status, SDL_SetClipboardText(report) ? "report copied to the clipboard"
                                                                                : "clipboard failed");
        } else if (k == SDLK_R) { /* replay from the label above the current line */
            uint32_t idx;
            if (rt->in.script && dbg_current_label(rt, &idx)) {
                char name[64];
                snprintf(name, sizeof name, "%s", rt->in.script->name);
                dbg_jump(rt, name, idx);
            }
        } else if (k == SDLK_ESCAPE) {
            d->mode = 0;
        }
        return true;
    }
    /* scene picker */
    const ft_script *s = dbg_picked_script(rt);
    int count = d->col == 0 ? (int)d->order_count : s ? (int)s->label_count : 0;
    int *sel = d->col == 0 ? &d->script : &d->label;
    switch (k) {
    case SDLK_UP: *sel -= 1; break;
    case SDLK_DOWN: *sel += 1; break;
    case SDLK_PAGEUP: *sel -= DBG_ROWS; break;
    case SDLK_PAGEDOWN: *sel += DBG_ROWS; break;
    case SDLK_HOME: *sel = 0; break;
    case SDLK_END: *sel = count - 1; break;
    case SDLK_LEFT: d->col = 0; break;
    case SDLK_RIGHT:
        if (s && s->label_count)
            d->col = 1;
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (s) {
            char name[64];
            snprintf(name, sizeof name, "%s", s->name);
            uint32_t line = d->col == 1 && d->label < (int)s->label_count ? s->labels[d->label].line : 0;
            dbg_jump(rt, name, line);
            d->mode = 0;
        }
        break;
    case SDLK_ESCAPE:
    case SDLK_BACKSPACE: d->mode = 1; break;
    default: break;
    }
    if (*sel >= count)
        *sel = count - 1;
    if (*sel < 0)
        *sel = 0;
    if (d->col == 0 && k != SDLK_RIGHT && k != SDLK_LEFT)
        d->label = 0;
    return true;
}

/* Draws a line of debug text through a small texture cache (the font is slow to render). */
static void dbg_text(ft_runtime *rt, float x, float y, const char *s, SDL_Color c)
{
    debug_ui *d = &rt->dbg;
    if (!s[0])
        return;
    dbg_entry *t = NULL, *oldest = &d->cache[0];
    for (int i = 0; i < DBG_CACHE && !t; i++) {
        if (d->cache[i].tex && strcmp(d->cache[i].s, s) == 0)
            t = &d->cache[i];
        else if (d->cache[i].used < oldest->used)
            oldest = &d->cache[i];
    }
    if (!t) {
        t = oldest;
        if (t->tex)
            SDL_DestroyTexture(t->tex);
        memset(t, 0, sizeof *t);
        snprintf(t->s, sizeof t->s, "%s", s);
        ft_image img;
        if (!ft_font_render(t->s, DBG_FONT_PX, &img))
            return;
        t->tex = texture_from(rt, &img);
        t->w = (float)img.w;
        t->h = (float)img.h;
        ft_image_free(&img);
        if (!t->tex)
            return;
    }
    t->used = ++d->tick;
    SDL_SetTextureColorMod(t->tex, c.r, c.g, c.b);
    SDL_FRect dst = { x, y, t->w, t->h };
    SDL_RenderTexture(rt->r, t->tex, NULL, &dst);
}

static void dbg_render(ft_runtime *rt)
{
    debug_ui *d = &rt->dbg;
    if (!d->mode)
        return;
    static const SDL_Color white = { 255, 255, 255, 255 }, dim = { 150, 160, 180, 255 },
                           hi = { 255, 220, 120, 255 }, ok = { 140, 230, 140, 255 };
    SDL_SetRenderDrawColor(rt->r, 0, 0, 0, d->mode == 2 ? 225 : 190);
    SDL_FRect all = { 0, 0, SCREEN_W, SCREEN_H };
    SDL_RenderFillRect(rt->r, &all);
    if (d->mode == 1) {
        dbg_text(rt, 16, 10, "dev   ` close   S scenes   R replay from label   C copy report   Ctrl fast-forward", hi);
        static char report[8192];
        dbg_report(rt, report, sizeof report);
        float y = 40;
        for (char *line = report; *line && y < SCREEN_H - 30; y += DBG_ROW) {
            char *end = strchr(line, '\n');
            char buf[160];
            size_t len = end ? (size_t)(end - line) : strlen(line);
            snprintf(buf, sizeof buf, "%.*s", (int)(len < sizeof buf - 1 ? len : sizeof buf - 1), line);
            dbg_text(rt, 16, y, buf, white);
            line = end ? end + 1 : line + len;
        }
    } else {
        dbg_text(rt, 16, 10, "scenes   Up/Down PgUp/PgDn pick   Left/Right column   Enter jump   Esc back", hi);
        int rows = DBG_ROWS;
        for (int col = 0; col < 2; col++) {
            const ft_script *s = dbg_picked_script(rt);
            int count = col == 0 ? (int)d->order_count : s ? (int)s->label_count : 0;
            int sel = col == 0 ? d->script : d->label;
            if (sel < d->top[col])
                d->top[col] = sel;
            if (sel >= d->top[col] + rows)
                d->top[col] = sel - rows + 1;
            if (col == 1 && d->col == 0)
                d->top[1] = 0;
            float x = col == 0 ? 16.0f : 260.0f;
            for (int i = d->top[col]; i < count && i < d->top[col] + rows; i++) {
                float y = 40.0f + (float)(i - d->top[col]) * DBG_ROW;
                char buf[160];
                if (col == 0) {
                    const ft_script *sc = &rt->assets.scripts[d->order[i]];
                    snprintf(buf, sizeof buf, "%s  (%u labels)", sc->name, sc->label_count);
                } else {
                    const ft_label *lb = &s->labels[i];
                    snprintf(buf, sizeof buf, "%s%s  (line %u)", label_glyph(lb->kind), lb->name, s->lines[lb->line].line_no);
                }
                if (i == sel) {
                    SDL_SetRenderDrawColor(rt->r, 50, 80, 150, d->col == col ? 255 : 120);
                    SDL_FRect r = { x - 4, y - 1, col == 0 ? 236.0f : SCREEN_W - x - 12, DBG_ROW };
                    SDL_RenderFillRect(rt->r, &r);
                }
                dbg_text(rt, x, y, buf, i == sel ? white : dim);
            }
        }
    }
    if (d->status[0])
        dbg_text(rt, 16, SCREEN_H - 26, d->status, ok);
}

void ft_runtime_event(ft_runtime *rt, const SDL_Event *ev)
{
    if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat &&
        (ev->key.key == SDLK_F12 || ev->key.key == SDLK_PRINTSCREEN)) {
        rt->want_shot = true; /* taken at the next render, from anywhere */
        return;
    }
    if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat && ev->key.key == SDLK_F11) {
        toggle_fullscreen(rt);
        return;
    }
    if (rt->dev && ev->type == SDL_EVENT_KEY_DOWN && dbg_key(rt, ev->key.key))
        return;
    if (rt->movie && ((ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat) || ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN)) {
        movie_stop(rt); /* any key or click skips the movie */
        return;
    }
    if (!rt->dbg.mode && ft_settingspanel_is_open(rt->settings)) {
        settings_event(rt, ev);
        return;
    }
    if (!rt->dbg.mode && ft_savepanel_is_open(rt->panel)) {
        panel_event(rt, ev);
        return;
    }
    if (!rt->dbg.mode && ft_titlemenu_is_open(rt->pause)) {
        pause_event(rt, ev);
        return;
    }
    if (!rt->dbg.mode && ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_ESCAPE && !ev->key.repeat) {
        pause_open(rt);
        return;
    }
    if (!rt->dbg.mode && ft_titlemenu_is_open(rt->title)) {
        int pick = ft_titlemenu_event(rt->title, ev);
        if (pick >= 0)
            rt->title_pick = rt->title_map[pick];
        return;
    }
    if (!rt->dbg.mode && ft_choicemenu_is_open(rt->menu)) {
        int pick = ft_choicemenu_event(rt->menu, ev);
        if (pick >= 0)
            rt->menu_pick = pick;
        return;
    }
    if (rt->dbg.mode && (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev->type == SDL_EVENT_MOUSE_MOTION))
        return;
    if (ev->type == SDL_EVENT_KEY_DOWN) {
        SDL_Keycode k = ev->key.key;
        if (k == SDLK_SPACE || k == SDLK_RETURN || k == SDLK_KP_ENTER)
            advance(rt);
    } else if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev->button.button == SDL_BUTTON_LEFT) {
        advance(rt);
    }
}

void ft_runtime_update(ft_runtime *rt)
{
    if (rt->paused) /* the Esc menu and what it opened take input; the game waits */
        return;
    uint64_t now = game_ms(rt);
    bool skip = rt->dev && (SDL_GetModState() & SDL_KMOD_CTRL) != 0; /* --dev: hold Ctrl to fast-forward */

    plane_tick(rt, &rt->bg, now);
    plane_tick(rt, &rt->event, now);
    for (int i = 0; i < BG02_LAYERS; i++)
        plane_tick(rt, &rt->bg02[i], now);
    film_tick(rt, &rt->film, now);
    /*
     * Catch up at most a few frames: the original loads pictures on the render thread, so a
     * slow load stalls fades and motion instead of skipping them.
     */
    uint64_t frame = now / FRAME_MS;
    if (frame - rt->last_frame > 3)
        rt->last_frame = frame - 3;
    for (; rt->last_frame < frame; rt->last_frame++)
        frame_step(rt, now);
    for (int i = 0; i < CH_COUNT; i++)
        channel_tick(&rt->ch[i]);

    if (rt->finished)
        return;
    if (ft_settingspanel_is_open(rt->settings)) { /* in place of system01, or over the save panel */
        if (!rt->autoplay)
            return;
        settings_event(rt, &(SDL_Event){ .key = { .type = SDL_EVENT_KEY_DOWN, .key = SDLK_ESCAPE } });
    }
    if (ft_savepanel_is_open(rt->panel)) { /* the script waits in savepanel.select */
        if (!rt->autoplay)
            return;
        panel_back(rt);
    }
    if (skip || rt->autoplay) {
        if (rt->autoplay && ft_choicemenu_is_open(rt->menu))
            rt->menu_pick = 0;
        if (rt->autoplay && ft_titlemenu_is_open(rt->title))
            rt->title_pick = rt->title_map[ft_titlemenu_selected(rt->title)];
        skip_wait(rt, now);
    }
    if (rt->clock_waiting) {
        if (clock_value(rt, now) < rt->clock_wait_target)
            return;
        rt->clock_waiting = false;
    }
    if (rt->movie && (rt->autoplay || !ft_movie_update(rt->movie, now)))
        movie_stop(rt);
    if (now < rt->hold_until_ms || now < rt->wait_until_ms || rt->sprite_blocking || rt->film.blocking ||
        rt->movie_waiting)
        return;
    if (rt->cmd_frame_wait) {
        if (rt->last_frame < rt->cmd_frame_target)
            return;
        rt->cmd_frame_wait = false;
    }

    /* Run lines until something waits; a frame never runs more than a bounded number. */
    for (int i = 0; i < 5000; i++) {
        ft_run r = ft_interp_step(&rt->in);
        if (r == FT_RUN_CONTINUE && !ft_settingspanel_is_open(rt->settings)) /* entering system01 opens it */
            continue;
        if (r == FT_RUN_END || r == FT_RUN_ERROR) {
            if (r == FT_RUN_ERROR)
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "script error: %s", rt->in.error);
            rt->finished = true;
        }
        break;
    }
}

/* Slot placement per %faceFormation (see FACE_SLOTS); `raise` lifts a slot above the base row. */
static void face_slot_pos(int formation, int slot, float h, float *x, float *raise)
{
    static const float centre = 416, left = 168, right = 664;
    static const float f4[4] = { 140, 248, 596, 704 };
    *raise = 0;
    *x = centre;
    switch (formation) {
    case 0:
    case 1:
        if (slot <= 3) {
            *x = (slot & 1) ? right : left;
            if (slot <= 1)
                *raise = h; /* 0/1 above 2/3 */
        }
        break;
    case 3:
        break;
    case 4:
        if (slot <= 3)
            *x = f4[slot];
        break;
    default: /* 2, the formation the game sets at boot */
        if (slot == 0)
            *x = left;
        else if (slot == 1)
            *x = right;
        else if (slot == 3)
            *raise = 544 - 200;
        break;
    }
}

static void draw_layer(ft_runtime *rt, const layer *l, float y, float max_w)
{
    if (!l->tex)
        return;
    float w = l->w, h = l->h;
    if (w > max_w) { /* larger tiers / atlases: fit to the screen width */
        h = h * max_w / w;
        w = max_w;
    }
    SDL_FRect dst = { (SCREEN_W - w) / 2, y, w, h };
    SDL_RenderTexture(rt->r, l->tex, NULL, &dst);
}

/*
 * The title screen's layers are placed in 3D by bg02.SetUserMatrix (not done yet), so we draw them
 * all centred at the top. That works for the background, the sky and the logo, but would put the
 * 特別編 stamp (BG02tl02, 278x95) over the logo's top line; the original has it right of and below
 * the logo's centre, so it gets a fixed spot under the logo's right end.
 */
#define TITLE_STAMP_X 690.0f
#define TITLE_STAMP_Y 236.0f

static void draw_page(ft_runtime *rt, const page *p)
{
    if (p->solid) {
        SDL_SetRenderDrawColor(rt->r, p->color.r, p->color.g, p->color.b, (Uint8)p->alpha);
        SDL_FRect all = { 0, 0, SCREEN_W, SCENE_H };
        SDL_RenderFillRect(rt->r, &all);
    } else if (p->pic.tex) {
        SDL_SetTextureAlphaMod(p->pic.tex, (Uint8)p->alpha);
        if (ft_stricmp(p->pic.code, "BG02tl02") == 0) {
            SDL_FRect dst = { TITLE_STAMP_X, TITLE_STAMP_Y, p->pic.w, p->pic.h };
            SDL_RenderTexture(rt->r, p->pic.tex, NULL, &dst);
        } else {
            draw_layer(rt, &p->pic, 0, SCREEN_W);
        }
    }
}

static void draw_plane(ft_runtime *rt, const plane *pl)
{
    draw_page(rt, &pl->cur);
    draw_page(rt, &pl->back);
}

/* Our frame for a face, in place of frmface1: a thin light border with a dark edge. */
static void draw_face_frame(ft_runtime *rt, float x, float y)
{
    SDL_FRect outer = { x - 2, y - 2, FACE_W + 4, FACE_H + 4 }, inner = { x - 1, y - 1, FACE_W + 2, FACE_H + 2 };
    SDL_SetRenderDrawColor(rt->r, 0, 0, 0, 200);
    SDL_RenderRect(rt->r, &outer);
    SDL_SetRenderDrawColor(rt->r, 255, 255, 255, 200);
    SDL_RenderRect(rt->r, &inner);
}

/*
 * Draw order follows the frame update (0x442ba0): bg (+0x9c0), %selSpeed, %sprite, %film,
 * event (+0xf20), bg02 layers 0-3, (+0xa9c), bg02 layers 4-9, then faces and the window.
 * So bg02 covers sprites, pans and films: scripts black them out with bg02.black and reveal
 * them with bg02.fadeout.
 */
static void draw_scene(ft_runtime *rt)
{
    SDL_SetRenderDrawBlendMode(rt->r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(rt->r, rt->backdrop.r, rt->backdrop.g, rt->backdrop.b, 255);
    SDL_RenderClear(rt->r);

    draw_plane(rt, &rt->bg);
    sel_render(rt);
    sprite_render(rt);
    /* film clips at their ZDF x/y within the CG area, in the units of their archive */
    for (int i = 0; !rt->film.hidden && i < rt->film.count; i++) {
        const film_clip *c = &rt->film.clip[i];
        if (!c->tex)
            continue;
        float k = c->scale;
        SDL_FRect dst = { SDL_roundf((float)c->zdf.x * k), SDL_roundf((float)c->zdf.y * k), c->w * k, c->h * k };
        SDL_RenderTexture(rt->r, c->tex, NULL, &dst);
    }
    draw_plane(rt, &rt->event);
    for (int i = 0; i < BG02_LAYERS; i++)
        draw_plane(rt, &rt->bg02[i]);
    if (rt->logo_state) { /* frame +0xe38, at (152, 84) native size (FUN_0040c170) */
        const page *pages[2] = { &rt->logo.cur, &rt->logo.back };
        for (int i = 0; i < 2; i++)
            if (pages[i]->pic.tex) {
                SDL_SetTextureAlphaMod(pages[i]->pic.tex, (Uint8)pages[i]->alpha);
                SDL_FRect dst = { 152, 84, pages[i]->pic.w, pages[i]->pic.h };
                SDL_RenderTexture(rt->r, pages[i]->pic.tex, NULL, &dst);
            }
    }
    for (int i = 0; i < FACE_SLOTS; i++) {
        const layer *f = &rt->face[i];
        if (!f->tex)
            continue;
        float x, raise = 0;
        face_slot_pos(rt->face_formation, i, FACE_H, &x, &raise);
        x += FACE_SHIFT_X;
        float y = FACE_Y - raise;
        draw_face_frame(rt, x, y);
        SDL_FRect dst = { x, y, FACE_W, FACE_H };
        SDL_RenderTexture(rt->r, f->tex, NULL, &dst);
    }

    if (rt->text_visible && rt->text) {
        float w = rt->text_w * rt->text_scale, h = rt->text_h * rt->text_scale;
        SDL_FRect dst = { SDL_floorf((SCREEN_W - w) / 2), TEXT_Y, w, h };
        SDL_RenderTexture(rt->r, rt->text, NULL, &dst);
    }
}

/* The scene as it is now, cropped to THUMB_SRC_H and shrunk to THUMB_W x THUMB_H, for a save. */
static SDL_Surface *scene_thumb(ft_runtime *rt)
{
    SDL_Texture *target = SDL_CreateTexture(rt->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, SCREEN_W, SCREEN_H);
    if (!target)
        return NULL;
    SDL_Texture *old = SDL_GetRenderTarget(rt->r);
    SDL_SetRenderTarget(rt->r, target);
    draw_scene(rt);
    SDL_Rect crop = { 0, 0, SCREEN_W, THUMB_SRC_H };
    SDL_Surface *full = SDL_RenderReadPixels(rt->r, &crop);
    SDL_SetRenderTarget(rt->r, old);
    SDL_DestroyTexture(target);
    SDL_Surface *small = full ? SDL_ScaleSurface(full, THUMB_W, THUMB_H, SDL_SCALEMODE_LINEAR) : NULL;
    SDL_DestroySurface(full);
    SDL_Surface *thumb = small ? SDL_ConvertSurface(small, SDL_PIXELFORMAT_RGB24) : NULL;
    SDL_DestroySurface(small);
    return thumb;
}

/* The thumbnails asked for: the panel's (the scene when it opened) and the last save point's. */
static void capture_thumbs(ft_runtime *rt)
{
    if (rt->want_thumb) {
        rt->want_thumb = false;
        SDL_DestroySurface(rt->thumb);
        rt->thumb = scene_thumb(rt);
    }
    if (rt->checkpoint.want_thumb) {
        rt->checkpoint.want_thumb = false;
        SDL_DestroySurface(rt->checkpoint.thumb);
        rt->checkpoint.thumb = scene_thumb(rt);
    }
}

/* Everything the player sees: the scene, the curtain and the topmost menu. */
static void draw_frame(ft_runtime *rt)
{
    draw_scene(rt);
    if (rt->movie) { /* on black, scaled to the screen width like 740-wide event art, centred */
        float w, h;
        ft_movie_size(rt->movie, &w, &h);
        SDL_FRect dst = { 0, 0, SCREEN_W, h * SCREEN_W / w };
        dst.y = SDL_floorf((SCREEN_H - dst.h) / 2);
        SDL_SetRenderDrawColor(rt->r, 0, 0, 0, 255);
        SDL_RenderClear(rt->r);
        ft_movie_render(rt->movie, &dst);
    }
    if (rt->curtain > 0) {
        SDL_SetRenderDrawColor(rt->r, 0, 0, 0, (Uint8)rt->curtain);
        SDL_RenderFillRect(rt->r, NULL);
    }

    /* only the topmost menu shows: the panels cover the Esc menu, which covers the game's menus */
    if (ft_settingspanel_is_open(rt->settings)) {
        ft_settingspanel_render(rt->settings);
    } else if (ft_savepanel_is_open(rt->panel)) {
        ft_savepanel_render(rt->panel);
    } else if (ft_titlemenu_is_open(rt->pause)) {
        SDL_SetRenderDrawColor(rt->r, 0, 0, 0, 160);
        SDL_RenderFillRect(rt->r, NULL);
        ft_titlemenu_render(rt->pause);
    } else {
        ft_titlemenu_render(rt->title);
        ft_choicemenu_render(rt->menu);
    }
}

/*
 * F12: the frame at the game's own 1024x768, whatever the window size, drawn off screen like the
 * save thumbnails, as screenshots/fantatech_<date>_<time>.png.
 */
static void take_screenshot(ft_runtime *rt)
{
    rt->want_shot = false;
    bool ok = false;
    char path[1200];
    SDL_Texture *target = SDL_CreateTexture(rt->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, SCREEN_W, SCREEN_H);
    if (target) {
        SDL_Texture *old = SDL_GetRenderTarget(rt->r);
        SDL_SetRenderTarget(rt->r, target);
        draw_frame(rt);
        SDL_Surface *shot = SDL_RenderReadPixels(rt->r, NULL);
        SDL_SetRenderTarget(rt->r, old);
        SDL_DestroyTexture(target);

        SDL_Time now = 0;
        SDL_DateTime dt = { 0 };
        if (SDL_GetCurrentTime(&now))
            SDL_TimeToDateTime(now, &dt, true);
        for (int n = 1; n < 100; n++) { /* a second shot in the same second gets _2, _3, ... */
            int len = snprintf(path, sizeof path, "%s/fantatech_%04d-%02d-%02d_%02d-%02d-%02d", rt->shot_dir, dt.year,
                               dt.month, dt.day, dt.hour, dt.minute, dt.second);
            snprintf(path + len, sizeof path - (size_t)len, n > 1 ? "_%d.png" : ".png", n);
            if (!SDL_GetPathInfo(path, NULL))
                break;
        }
        ok = shot && ft_mkdirs(rt->shot_dir) && SDL_SavePNG(shot, path);
        SDL_DestroySurface(shot);
    }
    if (ok)
        SDL_Log("screenshot: %s", path);
    else
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "screenshot: %s", SDL_GetError());
    const char *msg = ok ? "スクリーンショットを保存しました" : "スクリーンショットを保存できませんでした";
    const char *tr = ft_i18n_text(&rt->i18n, msg);
    if (rt->shot_notice)
        SDL_DestroyTexture(rt->shot_notice);
    rt->shot_notice = text_texture(rt, tr ? tr : msg, &rt->shot_notice_w, &rt->shot_notice_h);
    rt->shot_notice_until = SDL_GetTicks() + 2000;
}

void ft_runtime_render(ft_runtime *rt)
{
    capture_thumbs(rt);
    if (rt->want_shot)
        take_screenshot(rt);
    draw_frame(rt);
    if (rt->shot_notice && SDL_GetTicks() < rt->shot_notice_until) {
        SDL_FRect box = { 16, SCREEN_H - 16 - rt->shot_notice_h - 16, rt->shot_notice_w + 24, rt->shot_notice_h + 16 };
        SDL_SetRenderDrawColor(rt->r, 0, 0, 0, 190);
        SDL_RenderFillRect(rt->r, &box);
        SDL_FRect dst = { box.x + 12, box.y + 8, rt->shot_notice_w, rt->shot_notice_h };
        SDL_RenderTexture(rt->r, rt->shot_notice, NULL, &dst);
    }
    dbg_render(rt);
}
