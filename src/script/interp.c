#include "script/interp.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

const char *ft_midi_track(int n)
{
    static const char *const tracks[11] = { NULL, "as", "db", "tbst", "mtw4", "bfit", "amwt", "ssdt", "wifp", NULL, "samisi" };
    return n >= 1 && n <= 10 ? tracks[n] : NULL;
}

void ft_interp_init(ft_interp *in, const ft_host *host)
{
    /* Variables the engine itself sets (names referenced by mana01's command handlers). */
    static const char *const engine_vars[] = {
        "select.number", "select02.number", "selectCancel", "select02can", "number", "savedatanum",
        "savedataEmpty", "spriteLoadPAINTCount", "c1",
    };
    memset(in, 0, sizeof *in);
    in->host = *host;
    ft_vars_init(&in->vars);
    for (size_t i = 0; i < sizeof engine_vars / sizeof engine_vars[0]; i++)
        ft_vars_declare(&in->vars, engine_vars[i]);
}

static void clear_choices(ft_interp *in)
{
    for (int i = 0; i < in->choice_count; i++) {
        free(in->choices[i].label);
        free(in->choices[i].text);
    }
    in->choice_count = 0;
    in->menu = FT_MENU_NONE;
}

void ft_interp_free(ft_interp *in)
{
    clear_choices(in);
    ft_vars_free(&in->vars);
}

static ft_run fail(ft_interp *in, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(in->error, sizeof in->error, fmt, ap);
    va_end(ap);
    return FT_RUN_ERROR;
}

static void warn(ft_interp *in, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(in->error, sizeof in->error, fmt, ap);
    va_end(ap);
    in->warnings++;
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d)
        memcpy(d, s, n + 1);
    return d;
}

/* Script names may carry ".txt" (load game01.txt). */
static const ft_script *load_script(ft_interp *in, const char *name)
{
    char stem[64];
    snprintf(stem, sizeof stem, "%s", name);
    char *dot = strrchr(stem, '.');
    if (dot && ft_stricmp(dot, ".txt") == 0)
        *dot = 0;
    return in->host.load ? in->host.load(in->host.user, stem) : NULL;
}

/* Declarations (".name[n]", ".name=x") are handled when a script is entered. */
static void enter(ft_interp *in, const ft_script *s, uint32_t pc)
{
    in->script = s;
    in->pc = pc;
    for (uint32_t i = 0; i < s->count; i++)
        if (s->lines[i].kind == FT_LINE_DECL)
            ft_vars_declare_line(&in->vars, s->lines[i].head);
    if (in->host.entered)
        in->host.entered(in->host.user, in, s);
}

bool ft_interp_start(ft_interp *in, const char *name)
{
    const ft_script *s = load_script(in, name);
    if (!s) {
        fail(in, "no script %s", name);
        return false;
    }
    in->depth = 0;
    enter(in, s, 0);
    return true;
}

bool ft_interp_jump(ft_interp *in, const char *name, uint32_t line)
{
    const ft_script *s = load_script(in, name);
    if (!s) {
        fail(in, "no script %s", name);
        return false;
    }
    clear_choices(in);
    in->depth = 0;
    enter(in, s, line < s->count ? line : 0);
    return true;
}

/* Source line number -> line index; 0 stands for the end of the script. */
static int32_t line_index(const ft_script *s, uint32_t line_no)
{
    if (line_no == 0)
        return (int32_t)s->count;
    for (uint32_t i = 0; i < s->count; i++)
        if (s->lines[i].line_no == line_no)
            return (int32_t)i;
    return -1;
}

bool ft_interp_resume(ft_interp *in, const char *const *scripts, const uint32_t *line_nos, int count)
{
    if (count < 1 || count > FT_INTERP_STACK + 1) {
        fail(in, "bad saved call stack (%d frames)", count);
        return false;
    }
    const ft_script *s[FT_INTERP_STACK + 1];
    int32_t pc[FT_INTERP_STACK + 1];
    for (int i = 0; i < count; i++) {
        s[i] = load_script(in, scripts[i]);
        pc[i] = s[i] ? line_index(s[i], line_nos[i]) : -1;
        if (pc[i] < 0) {
            fail(in, "no line %u in script %s", line_nos[i], scripts[i]);
            return false;
        }
    }
    clear_choices(in);
    for (int i = 0; i < count - 1; i++) {
        in->stack[i].script = s[i];
        in->stack[i].pc = (uint32_t)pc[i];
    }
    in->depth = count - 1;
    enter(in, s[count - 1], (uint32_t)pc[count - 1]);
    return true;
}

/* Finds a label in the running script, then in the resident libraries. */
static int32_t label_of(ft_interp *in, const char *name, const ft_script **where)
{
    *where = in->script;
    int32_t t = ft_script_find_label(in->script, name, FT_LABEL_NONE);
    for (int i = 0; t < 0 && i < in->library_count; i++) {
        t = ft_script_find_label(in->libraries[i], name, FT_LABEL_NONE);
        *where = in->libraries[i];
    }
    if (t < 0)
        warn(in, "%s:%u: no label %s", in->script->name,
             in->pc < in->script->count ? in->script->lines[in->pc].line_no : 0, name);
    return t;
}

static ft_run jump(ft_interp *in, const char *label, bool call)
{
    const ft_script *where;
    int32_t t = label_of(in, label, &where);
    if (t < 0) {
        in->pc++;
        return FT_RUN_CONTINUE;
    }
    if (call) {
        if (in->depth == FT_INTERP_STACK)
            return fail(in, "%s: call stack overflow", in->script->name);
        in->stack[in->depth].script = in->script;
        in->stack[in->depth].pc = in->pc + 1;
        in->depth++;
    }
    in->script = where;
    in->pc = (uint32_t)t;
    return FT_RUN_CONTINUE;
}

static void merge(ft_interp *in, const ft_script *s)
{
    for (uint32_t i = 0; i < s->count; i++) {
        const ft_sline *m = &s->lines[i];
        if (m->kind == FT_LINE_DECL)
            ft_vars_declare_line(&in->vars, m->head);
        else if (m->kind == FT_LINE_TEXT || m->kind == FT_LINE_ASSIGN)
            ft_expr_exec(&in->vars, m->text);
    }
}

bool ft_interp_merge(ft_interp *in, const char *name)
{
    const ft_script *s = load_script(in, name);
    if (!s) {
        warn(in, "no script %s to merge", name);
        return false;
    }
    merge(in, s);
    return true;
}

bool ft_interp_add_library(ft_interp *in, const char *name)
{
    const ft_script *s = load_script(in, name);
    if (!s || in->library_count == FT_INTERP_LIBRARIES) {
        warn(in, "cannot add library %s", name);
        return false;
    }
    for (uint32_t i = 0; i < s->count; i++)
        if (s->lines[i].kind == FT_LINE_DECL)
            ft_vars_declare_line(&in->vars, s->lines[i].head);
    in->libraries[in->library_count++] = s;
    return true;
}

bool ft_interp_boot(ft_interp *in, const char *name)
{
    ft_interp_merge(in, "Ai10");
    ft_interp_add_library(in, "saveload");
    return ft_interp_start(in, name ? name : "start");
}

static bool add_choice(ft_interp *in, const char *label, const char *text, bool call)
{
    if (in->choice_count == FT_INTERP_CHOICES)
        return false;
    ft_choice *c = &in->choices[in->choice_count];
    c->label = dup_str(label ? label : "");
    c->text = dup_str(text ? text : (label ? label : ""));
    c->call = call;
    if (!c->label || !c->text)
        return false;
    in->choice_count++;
    return true;
}

/* Strips "..." from a select/select02 argument. */
static void unquote(char *dst, size_t cap, const char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"')
        snprintf(dst, cap, "%.*s", (int)(n - 2), s + 1);
    else
        snprintf(dst, cap, "%s", s);
}

/* Asks the host for a pick; on a pick, acts on it. Returns WAIT while unanswered. */
static ft_run resolve_menu(ft_interp *in)
{
    if (in->choice_count == 0) {
        in->menu = FT_MENU_NONE;
        in->pc++;
        return FT_RUN_CONTINUE;
    }
    int pick = in->host.choose ? in->host.choose(in->host.user, in, in->choices, in->choice_count) : 0;
    if (pick < 0)
        return FT_RUN_WAIT;
    if (pick >= in->choice_count)
        pick = in->choice_count - 1;
    int menu = in->menu;
    ft_choice c = in->choices[pick];
    in->menu = FT_MENU_NONE;
    ft_run r = FT_RUN_CONTINUE;
    if (menu == FT_MENU_SELECT_VAR) {
        ft_vars_set_int(&in->vars, in->menu_var, pick + 1);
        in->pc++;
    } else if (menu == FT_MENU_SELECT) {
        /* %select.set jumps to a ＊ choice label, falling back to a ○ location */
        int32_t t = ft_script_find_label(in->script, c.label, FT_LABEL_CHOICE);
        if (t < 0)
            t = ft_script_find_label(in->script, c.label, FT_LABEL_NONE);
        if (t < 0) {
            warn(in, "%s: no choice label %s", in->script->name, c.label);
            in->pc++;
        } else {
            in->pc = (uint32_t)t;
        }
    } else {
        r = jump(in, c.label, c.call);
    }
    /* select/select02 menus are rebuilt by each statement; %select and %cmnd keep theirs until cleared */
    if (menu == FT_MENU_SELECT_VAR)
        clear_choices(in);
    return r;
}

static bool head_is(const ft_sline *l, const char *s)
{
    return strcmp(l->head, s) == 0;
}

static bool head_starts(const ft_sline *l, const char *s)
{
    return strncmp(l->head, s, strlen(s)) == 0;
}

/* Returns true if the line was a built-in command; *r is the result. */
static bool builtin_command(ft_interp *in, const ft_sline *l, ft_run *r)
{
    const char *a0 = l->argc > 0 ? l->argv[0] : NULL;
    const char *a1 = l->argc > 1 ? l->argv[1] : NULL;
    *r = FT_RUN_CONTINUE;

    if (head_is(l, "%select.clear") || head_starts(l, "%cmnd.new") || head_is(l, "%cmnd.clear") ||
        head_is(l, "%cmnd.delete")) {
        clear_choices(in);
    } else if (head_is(l, "%select.set")) {
        add_choice(in, a0, a1, false);
    } else if (head_is(l, "%select.wait")) {
        in->menu = FT_MENU_SELECT;
        *r = resolve_menu(in);
        return true;
    } else if (head_starts(l, "%cmnd.") && !head_is(l, "%cmnd.wait")) {
        /* %cmnd.look / talk / move / think / system [.call] [.N] label */
        char verb[32];
        snprintf(verb, sizeof verb, "%s", l->head + 6);
        bool call = strstr(verb, ".call") != NULL;
        char text[160];
        snprintf(text, sizeof text, "%s: %s", verb, a0 ? a0 : "");
        add_choice(in, a0, text, call);
    } else if (head_is(l, "%cmnd.wait")) {
        in->menu = FT_MENU_CMND;
        *r = resolve_menu(in);
        return true;
    } else if ((head_is(l, "select") || head_is(l, "select02") || head_is(l, "selectCancel")) && l->argc > 0) {
        clear_choices(in);
        char text[256];
        for (int i = 0; i < l->argc; i++) {
            unquote(text, sizeof text, l->argv[i]);
            add_choice(in, NULL, text, false);
        }
        snprintf(in->menu_var, sizeof in->menu_var, "%s", head_is(l, "select02") ? "select02.number" : "select.number");
        in->menu = FT_MENU_SELECT_VAR;
        *r = resolve_menu(in);
        return true;
    } else {
        return false;
    }
    in->pc++;
    return true;
}

static ft_run exec_keyword(ft_interp *in, const ft_sline *l)
{
    const char *a0 = l->argc > 0 ? l->argv[0] : "";
    switch ((ft_keyword)l->keyword) {
    case FT_KW_GOTO:
        return jump(in, a0, false);
    case FT_KW_CALL:
        return jump(in, a0, true);
    case FT_KW_RETURN:
        if (in->depth == 0)
            return fail(in, "%s:%u: return without call", in->script->name, l->line_no);
        in->depth--;
        in->script = in->stack[in->depth].script;
        in->pc = in->stack[in->depth].pc;
        return FT_RUN_CONTINUE;
    case FT_KW_IF: {
        bool cond;
        if (strcmp(l->rest, "true") == 0)
            cond = true;
        else if (strcmp(l->rest, "false") == 0)
            cond = false;
        else
            cond = ft_expr_eval(&in->vars, l->rest, false) != 0;
        in->pc = cond ? in->pc + 1 : (uint32_t)l->block_end;
        return FT_RUN_CONTINUE;
    }
    case FT_KW_LOOP: {
        bool again = l->rest[0] == 0 || ft_expr_eval(&in->vars, l->rest, false) != 0;
        in->pc = again && l->loop_start >= 0 ? (uint32_t)l->loop_start : in->pc + 1;
        return FT_RUN_CONTINUE;
    }
    case FT_KW_LOAD:
    case FT_KW_LOADJUMP: {
        const ft_script *s = load_script(in, a0);
        if (!s)
            return fail(in, "%s:%u: no script %s", in->script->name, l->line_no, a0);
        in->depth = 0;
        clear_choices(in);
        enter(in, s, 0);
        if (l->keyword == FT_KW_LOADJUMP && l->argc > 1)
            return jump(in, l->argv[1], false);
        return FT_RUN_CONTINUE;
    }
    case FT_KW_LOADMERGE:
        /* Runs the other script's declarations (e.g. Ai10.txt's variable list). */
        ft_interp_merge(in, a0);
        in->pc++;
        return FT_RUN_CONTINUE;
    case FT_KW_END:
        return FT_RUN_END;
    case FT_KW_ON_GOTO:
    case FT_KW_ON_CALL: {
        int32_t n = ft_expr_eval(&in->vars, a0, false);
        if (n >= 1 && n < l->argc)
            return jump(in, l->argv[n], l->keyword == FT_KW_ON_CALL);
        in->pc++;
        return FT_RUN_CONTINUE;
    }
    default: /* sleep, dump */
        in->pc++;
        return FT_RUN_CONTINUE;
    }
}

bool ft_interp_goto(ft_interp *in, const char *label)
{
    if (!in->script)
        return false;
    const ft_script *where;
    int32_t t = label_of(in, label, &where);
    if (t < 0)
        return false;
    clear_choices(in);
    in->script = where;
    in->pc = (uint32_t)t;
    return true;
}

ft_run ft_interp_step(ft_interp *in)
{
    if (!in->script)
        return fail(in, "no script running");
    if (in->menu != FT_MENU_NONE)
        return resolve_menu(in);
    if (in->pc >= in->script->count)
        return FT_RUN_END;

    const ft_sline *l = &in->script->lines[in->pc];
    in->steps++;
    switch ((ft_line_kind)l->kind) {
    case FT_LINE_KEYWORD:
        return exec_keyword(in, l);
    case FT_LINE_ASSIGN:
        ft_expr_exec(&in->vars, l->text);
        in->pc++;
        return FT_RUN_CONTINUE;
    case FT_LINE_TEXT:
        /* kt_script_exec_fallback: a label name is an implicit call, else a statement */
        if (ft_script_find_label(in->script, l->head, FT_LABEL_NONE) >= 0)
            return jump(in, l->head, true);
        ft_expr_exec(&in->vars, l->text);
        in->pc++;
        return FT_RUN_CONTINUE;
    case FT_LINE_DIALOGUE: {
        in->pc++;
        bool wait = in->host.dialogue && in->host.dialogue(in->host.user, in, l);
        return wait ? FT_RUN_WAIT : FT_RUN_CONTINUE;
    }
    case FT_LINE_COMMAND: {
        ft_run r;
        if (builtin_command(in, l, &r))
            return r;
        in->pc++;
        bool wait = in->host.command && in->host.command(in->host.user, in, l);
        return wait ? FT_RUN_WAIT : FT_RUN_CONTINUE;
    }
    default: /* blank, label, declaration */
        in->pc++;
        return FT_RUN_CONTINUE;
    }
}

