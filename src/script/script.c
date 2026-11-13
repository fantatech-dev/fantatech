#include "script/script.h"

#include <stdlib.h>
#include <string.h>

/* UTF-8 spellings of the full-width markers. */
#define U_MARU "\xE2\x97\x8B"      /* ○ */
#define U_ASTER "\xEF\xBC\x8A"     /* ＊ */
#define U_STAR "\xE2\x98\x86"      /* ☆ */
#define U_SEMI "\xEF\xBC\x9B"      /* ； */
#define U_KAGI_OPEN "\xE3\x80\x8C" /* 「 */
#define U_KAGI_CLOSE "\xE3\x80\x8D"/* 」 */
#define U_SPACE "\xE3\x80\x80"     /* full-width space */
#define U_TOUTEN "\xE3\x80\x81"    /* 、 */

static const struct {
    const char *name;
    ft_keyword kw;
} keywords[] = {
    { "goto", FT_KW_GOTO },         { "call", FT_KW_CALL },       { "return", FT_KW_RETURN },
    { "if", FT_KW_IF },             { "loop", FT_KW_LOOP },       { "load", FT_KW_LOAD },
    { "end", FT_KW_END },           { "on.goto", FT_KW_ON_GOTO }, { "on.call", FT_KW_ON_CALL },
    { "loadjump", FT_KW_LOADJUMP }, { "loadMerge", FT_KW_LOADMERGE }, { "sleep", FT_KW_SLEEP },
    { "dump", FT_KW_DUMP },
};

/*
 * Bare (no '%', no '.') command words the dispatcher matches before falling back to text:
 * some by prefix (FUN_00431950), some exactly (FUN_00431910).
 */
static const char *const bare_command_prefixes[] = {
    "select", "configpanel", "commandbutton", "cover", "savepanel", "FSystem", "movieplay", "framewait",
    "CheckInitNow", "CheckMovieSkip", "CheckFullScreenNow",
};
static const char *const bare_commands[] = {
    "print", "OnKeyJump", "ResetOnKeyJump", "userwait", "bmpcheck", "dbghashi", "dbghashi2",
};

static bool starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* First occurrence of `needle` in [b, e), or NULL. */
static const char *find_in(const char *b, const char *e, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = b; p + n <= e; p++)
        if (memcmp(p, needle, n) == 0)
            return p;
    return NULL;
}

static char *dup_range(const char *b, const char *e)
{
    size_t n = (size_t)(e - b);
    char *s = malloc(n + 1);
    if (s) {
        memcpy(s, b, n);
        s[n] = 0;
    }
    return s;
}

static bool is_ws(const char *p, size_t *len)
{
    if (*p == ' ' || *p == '\t') {
        *len = 1;
        return true;
    }
    if (starts(p, U_SPACE)) {
        *len = 3;
        return true;
    }
    return false;
}

static const char *skip_ws(const char *p, const char *e)
{
    size_t n;
    while (p < e && is_ws(p, &n))
        p += n;
    return p;
}

static const char *trim_end(const char *b, const char *e)
{
    while (e > b) {
        if (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')
            e--;
        else if (e - b >= 3 && memcmp(e - 3, U_SPACE, 3) == 0)
            e -= 3;
        else
            break;
    }
    return e;
}

/*
 * End of the statement before an inline comment: ';' outside quotes and 「」, or '；' preceded
 * by whitespace.
 */
static const char *comment_start(const char *b, const char *e)
{
    bool quote = false;
    int kagi = 0;
    for (const char *p = b; p < e; p++) {
        if (*p == '"')
            quote = !quote;
        else if (!quote && starts(p, U_KAGI_OPEN))
            kagi++, p += 2;
        else if (!quote && starts(p, U_KAGI_CLOSE))
            kagi -= kagi > 0, p += 2;
        else if (!quote && !kagi && *p == ';')
            return p;
        else if (!quote && !kagi && starts(p, U_SEMI) && p > b && (p[-1] == ' ' || p[-1] == '\t'))
            return p;
    }
    return e;
}

/* Index of a top-level assignment '=' (not ==, !=, <=, >=), or NULL. */
static const char *find_assign(const char *b, const char *e)
{
    int depth = 0;
    bool quote = false;
    for (const char *p = b; p < e; p++) {
        if (*p == '"')
            quote = !quote;
        if (quote)
            continue;
        if (*p == '[' || *p == '(')
            depth++;
        else if ((*p == ']' || *p == ')') && depth > 0)
            depth--;
        else if (*p == '=' && depth == 0) {
            bool prev_op = p > b && (p[-1] == '=' || p[-1] == '!' || p[-1] == '<' || p[-1] == '>');
            bool next_eq = p + 1 < e && p[1] == '=';
            if (!prev_op && !next_eq)
                return p;
            if (next_eq)
                p++;
        }
    }
    return NULL;
}

/* An lvalue is a name, optionally indexed: no top-level whitespace other than around [ ]. */
static bool is_lvalue(const char *b, const char *e)
{
    e = trim_end(b, e);
    if (b == e || *b == '%' || *b == '"')
        return false;
    int depth = 0;
    for (const char *p = b; p < e; p++) {
        if (*p == '[')
            depth++;
        else if (*p == ']')
            depth--;
        else if (depth == 0 && (*p == ' ' || *p == '\t' || *p == ',' || *p == '(' || *p == '"'))
            return false;
    }
    return depth == 0;
}

static bool split_args(ft_sline *l)
{
    const char *b = l->rest, *e = l->rest + strlen(l->rest);
    int cap = 4, depth = 0;
    bool quote = false;
    l->argc = 0;
    l->argv = NULL;
    if (b == e)
        return true;
    l->argv = malloc(sizeof(char *) * (size_t)cap);
    if (!l->argv)
        return false;
    const char *start = b;
    for (const char *p = b;; p++) {
        bool end = p == e;
        if (!end) {
            if (*p == '"')
                quote = !quote;
            else if (!quote && (*p == '[' || *p == '('))
                depth++;
            else if (!quote && (*p == ']' || *p == ')') && depth > 0)
                depth--;
        }
        if (end || (!quote && depth == 0 && *p == ',')) {
            if (l->argc == cap) {
                cap *= 2;
                char **n = realloc(l->argv, sizeof(char *) * (size_t)cap);
                if (!n)
                    return false;
                l->argv = n;
            }
            const char *ab = skip_ws(start, p);
            char *arg = dup_range(ab, trim_end(ab, p));
            if (!arg)
                return false;
            l->argv[l->argc++] = arg;
            if (end)
                break;
            start = p + 1;
        }
    }
    return true;
}

static ft_keyword keyword_of(const char *head)
{
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++)
        if (strcmp(head, keywords[i].name) == 0)
            return keywords[i].kw;
    return FT_KW_NONE;
}

static bool is_bare_command(const char *head)
{
    for (size_t i = 0; i < sizeof bare_commands / sizeof bare_commands[0]; i++)
        if (strcmp(head, bare_commands[i]) == 0)
            return true;
    for (size_t i = 0; i < sizeof bare_command_prefixes / sizeof bare_command_prefixes[0]; i++)
        if (starts(head, bare_command_prefixes[i]))
            return true;
    return false;
}

/*
 * Dialogue: speaker「$id text」n. argv = { id, text, n } (id and n may be "").
 * Without 「 (narration with 、) the whole line is the text and there is no speaker.
 */
static bool parse_dialogue(ft_sline *l, const char *b, const char *open, const char *e)
{
    l->kind = FT_LINE_DIALOGUE;
    if (!open) {
        l->head = dup_range(b, b);
        l->rest = dup_range(b, e);
        l->argv = malloc(sizeof(char *) * 3);
        if (!l->head || !l->rest || !l->argv)
            return false;
        l->argv[0] = dup_range(b, b);
        l->argv[1] = dup_range(b, e);
        l->argv[2] = dup_range(b, b);
        l->argc = 3;
        return l->argv[0] && l->argv[1] && l->argv[2];
    }
    l->head = dup_range(b, trim_end(b, open));
    const char *body = open + 3;
    const char *close = e;
    for (const char *p = e - 3; p >= body; p--)
        if (starts(p, U_KAGI_CLOSE)) {
            close = p;
            break;
        }
    l->rest = dup_range(open, e);
    l->argv = malloc(sizeof(char *) * 3);
    if (!l->head || !l->rest || !l->argv)
        return false;
    const char *text = body;
    const char *id_end = body;
    if (*body == '$') {
        id_end = body + 1;
        while (id_end < close && *id_end != ' ' && *id_end != '\t' && !starts(id_end, U_SPACE))
            id_end++;
        text = skip_ws(id_end, close);
    }
    l->argv[0] = dup_range(*body == '$' ? body + 1 : body, id_end);
    l->argv[1] = dup_range(text, close);
    const char *n = close < e ? skip_ws(close + 3, e) : e;
    l->argv[2] = dup_range(n, e);
    l->argc = 3;
    return l->argv[0] && l->argv[1] && l->argv[2];
}

static bool parse_line(ft_sline *l, const char *b, const char *e)
{
    l->block_end = l->loop_start = -1;
    /* indentation */
    size_t n;
    while (b < e && is_ws(b, &n)) {
        l->indent++;
        b += n;
    }
    e = trim_end(b, e);
    if (b == e || *b == ';' || starts(b, U_SEMI)) {
        l->kind = FT_LINE_BLANK;
        l->text = dup_range(b, e);
        l->head = dup_range(b, b);
        l->rest = dup_range(b, b);
        return l->text && l->head && l->rest;
    }

    /* labels and declarations: the whole remainder is the name */
    ft_label_kind lk = FT_LABEL_NONE;
    size_t marker = 0;
    if (starts(b, U_MARU))
        lk = FT_LABEL_LOCATION, marker = 3;
    else if (starts(b, U_ASTER))
        lk = FT_LABEL_CHOICE, marker = 3;
    else if (starts(b, U_STAR))
        lk = FT_LABEL_SAVE, marker = 3;
    else if (*b == '#')
        lk = FT_LABEL_HASH, marker = 1;
    if (lk != FT_LABEL_NONE || *b == '.') {
        const char *ce = trim_end(b, comment_start(b, e));
        l->kind = lk != FT_LABEL_NONE ? FT_LINE_LABEL : FT_LINE_DECL;
        l->label_kind = (uint8_t)lk;
        l->text = dup_range(b, ce);
        const char *nb = skip_ws(b + (lk != FT_LABEL_NONE ? marker : 1), ce);
        l->head = dup_range(nb, ce);
        l->rest = dup_range(ce, ce);
        return l->text && l->head && l->rest;
    }

    /* head = first token (up to whitespace) */
    const char *he = b;
    while (he < e && !is_ws(he, &n))
        he++;
    char *head_tok = dup_range(b, he);
    if (!head_tok)
        return false;
    bool commandish = *b == '%' || keyword_of(head_tok) != FT_KW_NONE || is_bare_command(head_tok) ||
                      memchr(head_tok, '.', strlen(head_tok)) != NULL;
    free(head_tok);

    /*
     * Dialogue: like kt_script_dialogue_kind (0x4184c0), any non-command line whose first
     * token contains 「 or 、. Its text is kept verbatim (no comment stripping inside 「」).
     */
    if (!commandish && (find_in(b, he, U_KAGI_OPEN) || find_in(b, he, U_TOUTEN))) {
        const char *ce = trim_end(b, comment_start(b, e));
        l->text = dup_range(b, ce);
        const char *open = find_in(b, ce, U_KAGI_OPEN);
        return l->text && parse_dialogue(l, b, open, ce);
    }

    e = trim_end(b, comment_start(b, e));
    he = he < e ? he : e;
    l->text = dup_range(b, e);
    if (!l->text)
        return false;

    const char *eq = find_assign(b, e);
    if (eq && *b != '%' && is_lvalue(b, eq)) {
        l->kind = FT_LINE_ASSIGN;
        l->head = dup_range(b, trim_end(b, eq));
        const char *rb = skip_ws(eq + 1, e);
        l->rest = dup_range(rb, e);
    } else {
        /* "%cmd=value" takes its argument after '=' */
        const char *cmd_end = he;
        if (*b == '%') {
            const char *q = memchr(b, '=', (size_t)(he - b));
            if (q)
                cmd_end = q;
        }
        l->head = dup_range(b, cmd_end);
        const char *rb = skip_ws(cmd_end < he ? cmd_end + 1 : he, e);
        l->rest = dup_range(rb, e);
        if (!l->head || !l->rest)
            return false;
        l->keyword = (uint8_t)keyword_of(l->head);
        if (l->keyword != FT_KW_NONE)
            l->kind = FT_LINE_KEYWORD;
        else if (*b == '%' || strchr(l->head, '.') || is_bare_command(l->head))
            l->kind = FT_LINE_COMMAND;
        else
            l->kind = FT_LINE_TEXT;
    }
    if (!l->head || !l->rest)
        return false;
    return split_args(l);
}

static bool is_code(const ft_sline *l)
{
    return l->kind != FT_LINE_BLANK;
}

static void resolve_blocks(ft_script *s)
{
    for (uint32_t i = 0; i < s->count; i++) {
        ft_sline *l = &s->lines[i];
        if (l->kind != FT_LINE_KEYWORD)
            continue;
        if (l->keyword == FT_KW_IF) {
            uint32_t j = i + 1;
            while (j < s->count && (!is_code(&s->lines[j]) || s->lines[j].indent > l->indent))
                j++;
            l->block_end = (int32_t)j;
        } else if (l->keyword == FT_KW_LOOP) {
            int32_t start = -1;
            for (int32_t j = (int32_t)i - 1; j >= 0; j--) {
                const ft_sline *p = &s->lines[j];
                if (!is_code(p))
                    continue;
                if (p->indent <= l->indent)
                    break;
                start = j;
            }
            l->loop_start = start;
        }
    }
}

static bool collect_labels(ft_script *s)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->count; i++)
        n += s->lines[i].kind == FT_LINE_LABEL;
    s->labels = calloc(n ? n : 1, sizeof *s->labels);
    if (!s->labels)
        return false;
    for (uint32_t i = 0; i < s->count; i++) {
        const ft_sline *l = &s->lines[i];
        if (l->kind != FT_LINE_LABEL)
            continue;
        ft_label *lab = &s->labels[s->label_count++];
        lab->name = l->head;
        lab->line = i;
        lab->kind = l->label_kind;
    }
    return true;
}

bool ft_script_parse(ft_script *s, const char *name, const char *utf8, size_t len)
{
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    uint32_t lines = 1;
    for (size_t i = 0; i < len; i++)
        lines += utf8[i] == '\n';
    s->lines = calloc(lines, sizeof *s->lines);
    if (!s->lines) {
        ft_set_error("out of memory parsing %s", name);
        return false;
    }
    const char *p = utf8, *end = utf8 + len;
    while (p <= end && s->count < lines) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        ft_sline *l = &s->lines[s->count];
        l->line_no = s->count + 1;
        s->count++;
        if (!parse_line(l, p, le)) {
            ft_set_error("out of memory parsing %s line %u", name, l->line_no);
            ft_script_free(s);
            return false;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    resolve_blocks(s);
    if (!collect_labels(s)) {
        ft_set_error("out of memory parsing %s", name);
        ft_script_free(s);
        return false;
    }
    return true;
}

void ft_script_free(ft_script *s)
{
    for (uint32_t i = 0; i < s->count; i++) {
        ft_sline *l = &s->lines[i];
        free(l->text);
        free(l->head);
        free(l->rest);
        for (int a = 0; a < l->argc; a++)
            free(l->argv[a]);
        free(l->argv);
    }
    free(s->lines);
    free(s->labels);
    memset(s, 0, sizeof *s);
}

int32_t ft_script_find_label(const ft_script *s, const char *name, ft_label_kind kind)
{
    for (uint32_t i = 0; i < s->label_count; i++)
        if ((kind == FT_LABEL_NONE || s->labels[i].kind == kind) && ft_stricmp(s->labels[i].name, name) == 0)
            return (int32_t)s->labels[i].line;
    return -1;
}

const char *ft_line_kind_name(ft_line_kind k)
{
    static const char *const names[] = { "blank", "label", "decl", "keyword", "command", "assign", "dialogue", "text" };
    return (unsigned)k < sizeof names / sizeof names[0] ? names[k] : "?";
}
