/*
 * fantatech — reimplementation of the Kotetsu engine. Entry point.
 *
 * usage: fantatech [game dir] [script] [--lang xx] [--dev] [--shot out.png (--at N | --after-ms T)]
 *        fantatech --install <disc.iso> [dir]
 *        fantatech --version
 *        fantatech <disc.iso>          (or drop the ISO on the exe: installs to mana01\)
 *   <game dir>  the folder holding text.idx, voice.bin, exec\, ... (an installed copy);
 *               defaults to mana01\ next to the exe
 *   --install   unpack the game from the original disc image into dir (default: mana01\
 *               next to the exe), checking every file's MD5, then exit
 *   [script]    start from this script instead of start.txt (e.g. starmenu, Ava011)
 *   --lang xx   show translations from i18n/xx.txt (or FT_LANG=xx)
 *   --dev       developer tools: the ` overlay and Ctrl fast-forward (or FT_DEV=1)
 *   --shot      save a frame and exit: --at N autoplays until dialogue line N is on screen;
 *               --after-ms T plays in real time without input and captures after T ms
 *
 * Space / Enter / click advances, Up / Down picks in menus, Esc opens the menu (save, load,
 * settings, quit), F11 toggles fullscreen (the default; also in the settings), F2 takes a
 * screenshot.
 */

#include "core/ft_common.h"
#include "game/runtime.h"
#include "tools/png_write.h"
#include "vfs/install.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define FT_SCREEN_W 1024
#define FT_SCREEN_H 768

static bool save_frame(SDL_Renderer *renderer, const char *path)
{
    SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL);
    SDL_Surface *s = raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_BGRA32) : NULL;
    bool ok = false;
    ft_image img;
    if (s && ft_image_alloc(&img, (uint32_t)s->w, (uint32_t)s->h)) {
        for (int y = 0; y < s->h; y++)
            memcpy(img.bgra + (size_t)y * (size_t)s->w * 4, (const uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch,
                   (size_t)s->w * 4);
        for (size_t i = 0; i < (size_t)s->w * (size_t)s->h; i++)
            img.bgra[i * 4 + 3] = 255;
        ok = ft_png_write(path, &img);
        ft_image_free(&img);
    }
    SDL_DestroySurface(s);
    SDL_DestroySurface(raw);
    if (!ok)
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "screenshot failed: %s", SDL_GetError());
    return ok;
}

/* mana01\ next to the exe: where --install puts the game and where we look for it by default. */
static void default_game_dir(char *out, size_t cap)
{
    const char *base = SDL_GetBasePath();
    snprintf(out, cap, "%smana01", base ? base : "./");
}

/* An installed game: the folder has text.idx. */
static bool has_game(const char *dir)
{
    if (!dir)
        return false;
    char probe[1100];
    snprintf(probe, sizeof probe, "%s/text.idx", dir);
    FILE *f = ft_fopen(probe, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

typedef struct install_meter {
    const char *file;
    int percent;
} install_meter;

static void install_progress(void *user, const char *file, uint64_t done, uint64_t total)
{
    install_meter *m = user;
    int percent = (int)(done * 100 / total);
    if (file == m->file && percent == m->percent)
        return;
    m->file = file;
    m->percent = percent;
    printf("\r%3d%%  %-40s", percent, file);
    fflush(stdout);
}

/*
 * On Windows the exe is a GUI app, so playing opens no console window. Output still reaches a
 * terminal we were started from (attached here), or wherever it's redirected (inherited
 * handles, left alone). `own` opens a console window of our own when there's neither: the
 * install does, so a drag-and-drop shows its progress.
 */
static bool g_console; /* our output reaches someone: a terminal, a file or a pipe */

static void console_open(bool own)
{
#ifdef _WIN32
    if (g_console)
        return;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) {
        g_console = true; /* redirected to a file or pipe */
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !(own && AllocConsole()))
        return;
    FILE *f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    SetConsoleOutputCP(CP_UTF8); /* paths and messages are UTF-8 */
    g_console = true;
#else
    (void)own;
    g_console = true;
#endif
}

/*
 * Errors before the game window opens: logged, and shown in a message box when nobody would
 * see the log (a player who double-clicked the exe). Scripted runs never block on a box.
 */
static void fail(const char *fmt, ...)
{
    char msg[1600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    SDL_Log("%s", msg);
    if (!g_console)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "fantatech", msg, NULL);
}

/* Launched by dropping a file on the exe, the console is ours alone and closes with us: keep the result readable. */
static void pause_if_own_console(void)
{
#ifdef _WIN32
    DWORD ids[2];
    if (GetConsoleProcessList(ids, 2) == 1) {
        printf("Press Enter to close.");
        fflush(stdout);
        (void)getchar();
    }
#endif
}

static bool is_iso_path(const char *s)
{
    size_t n = strlen(s);
    return n > 4 && SDL_strcasecmp(s + n - 4, ".iso") == 0;
}

static int install(const char *iso, const char *dir)
{
    console_open(true);
    char def[1024];
    if (!dir) {
        default_game_dir(def, sizeof def);
        dir = def;
    }
    printf("Installing from %s into %s\n", iso, dir);
    install_meter m = { NULL, -1 };
    uint64_t t0 = SDL_GetTicks();
    int rc = 0;
    if (!ft_install_from_iso(iso, dir, install_progress, &m)) {
        printf("\ninstall failed: %s\n", ft_get_error());
        rc = 1;
    } else {
        printf("\rDone in %.0f s. All files passed their MD5 check.%-20s\n", (double)(SDL_GetTicks() - t0) / 1000.0,
               "");
    }
    pause_if_own_console();
    return rc;
}

int main(int argc, char **argv)
{
    const char *game_dir = NULL, *script = NULL, *shot = NULL, *label = NULL, *lang = SDL_getenv("FT_LANG");
    uint32_t shot_at = 1;
    uint64_t shot_after_ms = 0;
    bool dev = false;
    console_open(false); /* the terminal we were started from, if any */
    /* a lone .iso (dropped on the exe) installs it next to the exe */
    if (argc == 2 && is_iso_path(argv[1]))
        return install(argv[1], NULL);
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("fantatech %s\n", FT_VERSION);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--install") == 0) {
            if (i + 1 >= argc) {
                console_open(true);
                printf("usage: %s --install <disc.iso> [dir]\n", argv[0]);
                pause_if_own_console();
                return 1;
            }
            const char *dir = i + 2 < argc && strncmp(argv[i + 2], "--", 2) != 0 ? argv[i + 2] : NULL;
            return install(argv[i + 1], dir);
        }
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc)
            shot = argv[++i];
        else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc)
            lang = argv[++i];
        else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc)
            label = argv[++i];
        else if (strcmp(argv[i], "--dev") == 0)
            dev = true;
        else if (strcmp(argv[i], "--at") == 0 && i + 1 < argc)
            shot_at = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--after-ms") == 0 && i + 1 < argc)
            shot_after_ms = strtoull(argv[++i], NULL, 10);
        else if (!game_dir)
            game_dir = argv[i];
        else if (!script)
            script = argv[i];
    }
    /* the first word is the game dir only if it holds the game; else it's the script and the
     * game comes from mana01\ next to the exe */
    char def_dir[1024];
    if (!has_game(game_dir)) {
        default_game_dir(def_dir, sizeof def_dir);
        if (!has_game(def_dir)) {
            fail("No game found in %s.\n\n"
                 "Drop the game's disc image (.iso) on fantatech.exe to install it there, "
                 "or pass the game folder: fantatech <game dir> [script].",
                 def_dir);
            return 1;
        }
        if (game_dir && !script)
            script = game_dir;
        game_dir = def_dir;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        fail("SDL_Init: %s", SDL_GetError());
        return 1;
    }

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    /* hidden until the saved display setting is applied, so fullscreen doesn't flash a window first */
    if (!SDL_CreateWindowAndRenderer("fantatech " FT_VERSION, FT_SCREEN_W, FT_SCREEN_H, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN,
                                     &window, &renderer)) {
        fail("Can't open the game window: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderLogicalPresentation(renderer, FT_SCREEN_W, FT_SCREEN_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(renderer, 1);

    ft_runtime *rt = ft_runtime_create(renderer, game_dir, script);
    if (!rt) {
        fail("Can't load the game from %s:\n%s", game_dir, ft_get_error());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    if (!shot) /* captures stay a 1024x768 window */
        ft_runtime_apply_display(rt);
    SDL_ShowWindow(window);
    if (lang)
        ft_runtime_set_language(rt, lang);
    const char *dev_env = SDL_getenv("FT_DEV");
    ft_runtime_set_dev(rt, dev || (dev_env && dev_env[0] == '1'));
    if (label && !ft_runtime_jump_label(rt, label))
        SDL_Log("no label %s in the start script", label);
    /* --at N autoplays to line N; --after-ms T plays in real time without input for T ms */
    if (shot && !shot_after_ms)
        ft_runtime_set_autoplay(rt, true);
    uint64_t start_ms = SDL_GetTicks();

    int rc = 0;
    bool running = true;
    while (running && !ft_runtime_finished(rt)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT)
                running = false;
            else
                ft_runtime_event(rt, &ev);
        }
        ft_runtime_update(rt);
        ft_runtime_render(rt);
        bool due = shot_after_ms ? SDL_GetTicks() - start_ms >= shot_after_ms : ft_runtime_dialogue_count(rt) >= shot_at;
        if (shot && due) {
            SDL_Log("captured after %llu ms, %u dialogue lines shown", (unsigned long long)(SDL_GetTicks() - start_ms),
                    ft_runtime_dialogue_count(rt));
            rc = save_frame(renderer, shot) ? 0 : 1;
            running = false;
        }
        SDL_RenderPresent(renderer);
    }
    if (shot && running)
        rc = 1; /* scripts ended before line N */

    ft_runtime_destroy(rt);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return rc;
}
