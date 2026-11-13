/*
 * Script interpreter: runs parsed scripts line by line.
 *
 * Handles flow (labels, goto/call/return, if/loop blocks, load, on.goto/on.call),
 * variables, and the menu commands (%select.*, select/select02, %cmnd.*). Dialogue, menu
 * choices and every other command go to the host, so the same core drives both the SDL
 * runtime and ftconv's headless trace.
 */
#pragma once

#include "script/expr.h"
#include "script/script.h"

typedef struct ft_interp ft_interp;

typedef enum ft_run {
    FT_RUN_CONTINUE,    /* step done, keep going */
    FT_RUN_WAIT,        /* host asked to wait (dialogue, timer, choice pending, ...) */
    FT_RUN_END,         /* end of script / "end" */
    FT_RUN_ERROR,       /* fatal: missing script, return without call, ... (see error) */
} ft_run;

typedef struct ft_choice {
    char *label;        /* jump target */
    char *text;         /* shown text */
    bool call;          /* %cmnd.*.call: call instead of goto */
} ft_choice;

typedef struct ft_host {
    void *user;
    /* Returns the parsed script named `name` (no extension), or NULL. Owned by the host. */
    const ft_script *(*load)(void *user, const char *name);
    /* A dialogue line; return true to wait (the usual case). */
    bool (*dialogue)(void *user, ft_interp *in, const ft_sline *line);
    /* Pick a menu entry: return its index, or -1 to wait and be asked again. */
    int (*choose)(void *user, ft_interp *in, const ft_choice *choices, int count);
    /* Any command the interpreter doesn't handle; return true to wait. */
    bool (*command)(void *user, ft_interp *in, const ft_sline *line);
    /* Optional: a script was entered (load / loadjump), for tracing. */
    void (*entered)(void *user, ft_interp *in, const ft_script *s);
} ft_host;

#define FT_INTERP_STACK 64
#define FT_INTERP_CHOICES 32
#define FT_INTERP_LIBRARIES 4

struct ft_interp {
    ft_host host;
    ft_vars vars;
    const ft_script *script;
    uint32_t pc;
    struct {
        const ft_script *script;
        uint32_t pc;
    } stack[FT_INTERP_STACK];
    int depth;

    ft_choice choices[FT_INTERP_CHOICES];
    int choice_count;
    enum { FT_MENU_NONE, FT_MENU_SELECT, FT_MENU_SELECT_VAR, FT_MENU_CMND } menu; /* menu awaiting a pick */
    char menu_var[32];          /* select.number / select02.number */

    const ft_script *libraries[FT_INTERP_LIBRARIES]; /* resident scripts searched for labels */
    int library_count;

    uint64_t steps;
    uint32_t warnings;          /* unresolved labels and similar non-fatal problems */
    char error[256];
};

void ft_interp_init(ft_interp *in, const ft_host *host);
void ft_interp_free(ft_interp *in);

/* Runs a script's declarations into the variable table (like "loadMerge"). */
bool ft_interp_merge(ft_interp *in, const char *name);

/*
 * Merges a script and keeps it resident: labels missing from the running script are
 * looked up in libraries (e.g. saveload.txt's "システム" routines that %cmnd.system.call uses).
 */
bool ft_interp_add_library(ft_interp *in, const char *name);

/*
 * Boots like the engine: merges Ai10 (variable declarations), keeps saveload resident,
 * then starts `name` ("start" when NULL).
 */
bool ft_interp_boot(ft_interp *in, const char *name);

/* Starts running `name` from its first line. */
bool ft_interp_start(ft_interp *in, const char *name);

/* Starts `name` at line index `line`, dropping the call stack and any pending menu (debug jumps). */
bool ft_interp_jump(ft_interp *in, const char *name, uint32_t line);

/*
 * Restores a saved call stack: frame i runs `scripts[i]` at the line with source line number
 * `line_nos[i]` (0: its end); the last frame is where execution resumes, the others are return addresses.
 * Drops any pending menu. Fails (changing nothing) if a script or line no longer exists.
 */
bool ft_interp_resume(ft_interp *in, const char *const *scripts, const uint32_t *line_nos, int count);

/* Jumps to a label (running script, then libraries), dropping any pending menu. */
bool ft_interp_goto(ft_interp *in, const char *label);

/* Executes one line. */
ft_run ft_interp_step(ft_interp *in);

/* %midi.play n -> music.bin entry stem (table at 0x4efbe4 in mana01), or NULL (0 = stop, 9 = none). */
const char *ft_midi_track(int n);
