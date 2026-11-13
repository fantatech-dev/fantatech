/*
 * Save files (our format, not the original's): one UTF-8 text file per slot, NN.sav, with a
 * screenshot beside it as NN.png (written by the runtime).
 *
 *   fantatech-save 1
 *   time <unix seconds>
 *   title <save point name>
 *   frame <script> <source line>      call stack, bottom first; the last one is where play resumes
 *   set <key> <value>                 runtime state (pictures, faces, music), see runtime.c
 *   var <name> <int>
 *   str <name> <text to end of line>  (after its var line)
 *   arr <name> <n> <v0> ... <vn-1>
 *
 * Saves are taken at the system verb of a command menu, so the call stack, the variables and
 * what's on screen are all the state there is: resuming re-runs the menu block.
 */
#pragma once

#include "script/expr.h"

#define FT_SAVE_SLOTS 48
#define FT_SAVE_FRAMES 16
#define FT_SAVE_KV 48

typedef struct ft_save_frame {
    char script[64];
    uint32_t line_no;
} ft_save_frame;

typedef struct ft_save {
    int64_t time;
    char title[128];
    ft_save_frame frame[FT_SAVE_FRAMES];
    int frames;
    struct {
        char key[32];
        char value[96];
    } kv[FT_SAVE_KV];
    int kv_count;
    ft_vars vars;               /* filled by ft_save_read with vars = true */
} ft_save;

/* Writes `s` with the variables `vars` (s->vars is ignored), replacing the file safely. */
bool ft_save_write(const char *path, const ft_save *s, const ft_vars *vars);

/* Reads a save; `vars` false skips the variables (enough for the slot list). */
bool ft_save_read(const char *path, ft_save *s, bool vars);
void ft_save_free(ft_save *s);

void ft_save_set(ft_save *s, const char *key, const char *value);
/* The value stored under `key`, or NULL. */
const char *ft_save_get(const ft_save *s, const char *key);

/* <dir>/NN.<ext> for slot n (1-based). */
void ft_save_path(char *out, size_t cap, const char *dir, int slot, const char *ext);
