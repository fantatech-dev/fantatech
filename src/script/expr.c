#include "script/expr.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ---- variables ---------------------------------------------------------- */

void ft_vars_init(ft_vars *vs)
{
    memset(vs, 0, sizeof *vs);
}

void ft_vars_free(ft_vars *vs)
{
    for (uint32_t i = 0; i < vs->count; i++) {
        free(vs->v[i].name);
        free(vs->v[i].str);
        free(vs->v[i].array);
    }
    free(vs->v);
    memset(vs, 0, sizeof *vs);
}

static void *dup_mem(const void *p, size_t n)
{
    void *d = p && n ? malloc(n) : NULL;
    if (d)
        memcpy(d, p, n);
    return d;
}

bool ft_vars_copy(ft_vars *dst, const ft_vars *src)
{
    ft_vars_free(dst);
    if (src->count) {
        dst->v = calloc(src->count, sizeof *dst->v);
        if (!dst->v)
            return false;
        dst->cap = src->count;
    }
    for (uint32_t i = 0; i < src->count; i++) {
        const ft_var *s = &src->v[i];
        ft_var *d = &dst->v[i];
        dst->count = i + 1; /* so a failure part way still frees what was copied */
        d->value = s->value;
        d->array_len = s->array_len;
        d->name = dup_mem(s->name, strlen(s->name) + 1);
        d->str = s->str ? dup_mem(s->str, strlen(s->str) + 1) : NULL;
        d->array = s->array ? dup_mem(s->array, sizeof *s->array * s->array_len) : NULL;
        if (!d->name || (s->str && !d->str) || (s->array && s->array_len && !d->array)) {
            ft_vars_free(dst);
            return false;
        }
    }
    return true;
}

static void var_error(ft_vars *vs, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vs->last_error, sizeof vs->last_error, fmt, ap);
    va_end(ap);
    vs->errors++;
}

ft_var *ft_vars_find(ft_vars *vs, const char *name)
{
    for (uint32_t i = 0; i < vs->count; i++)
        if (ft_stricmp(vs->v[i].name, name) == 0)
            return &vs->v[i];
    return NULL;
}

ft_var *ft_vars_declare(ft_vars *vs, const char *name)
{
    ft_var *v = ft_vars_find(vs, name);
    if (v)
        return v;
    if (vs->count == vs->cap) {
        uint32_t cap = vs->cap ? vs->cap * 2 : 64;
        ft_var *n = realloc(vs->v, sizeof *n * cap);
        if (!n)
            return NULL;
        vs->v = n;
        vs->cap = cap;
    }
    v = &vs->v[vs->count];
    memset(v, 0, sizeof *v);
    size_t len = strlen(name);
    v->name = malloc(len + 1);
    if (!v->name)
        return NULL;
    memcpy(v->name, name, len + 1);
    vs->count++;
    return v;
}

void ft_vars_set_int(ft_vars *vs, const char *name, int32_t value)
{
    ft_var *v = ft_vars_declare(vs, name);
    if (v)
        v->value = value;
}

void ft_vars_set_str(ft_vars *vs, const char *name, const char *str)
{
    ft_var *v = ft_vars_declare(vs, name);
    if (!v)
        return;
    size_t len = strlen(str);
    char *s = malloc(len + 1);
    if (!s)
        return;
    memcpy(s, str, len + 1);
    free(v->str);
    v->str = s;
}

static bool var_resize(ft_var *v, uint32_t len)
{
    if (len <= v->array_len)
        return true;
    int32_t *a = realloc(v->array, sizeof *a * len);
    if (!a)
        return false;
    memset(a + v->array_len, 0, sizeof *a * (len - v->array_len));
    v->array = a;
    v->array_len = len;
    return true;
}

bool ft_vars_declare_line(ft_vars *vs, const char *decl)
{
    const char *br = strchr(decl, '[');
    if (br && !strchr(decl, '=')) {
        char name[128];
        size_t n = (size_t)(br - decl);
        while (n && (decl[n - 1] == ' ' || decl[n - 1] == '\t'))
            n--;
        if (n == 0 || n >= sizeof name)
            return false;
        memcpy(name, decl, n);
        name[n] = 0;
        long len = strtol(br + 1, NULL, 10);
        ft_var *v = ft_vars_declare(vs, name);
        return v && len > 0 && var_resize(v, (uint32_t)len);
    }
    return ft_expr_exec(vs, decl);
}

/* ---- tokens ------------------------------------------------------------- */

typedef enum tok_kind { T_END, T_NUM, T_STR, T_NAME, T_OP } tok_kind;

typedef struct tok {
    tok_kind kind;
    const char *b, *e;   /* source text (names, strings without quotes) */
    int32_t num;
    int op;              /* T_OP: one of the OP_ values or '(' ')' '[' ']' */
} tok;

enum { OP_ADD = 2, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_LE, OP_GE, OP_EQ, OP_NE, OP_LT, OP_GT, OP_AND, OP_OR };

typedef struct lexer {
    const char *p, *end;
    tok cur;
    ft_vars *vs;
    bool declare;
    bool bad;
} lexer;

static bool op_char(char c)
{
    return strchr("+-*/%<>=!&|()[]\"", c) != NULL;
}

static void lex(lexer *L)
{
    tok *t = &L->cur;
    while (L->p < L->end) {
        if (*L->p == ' ' || *L->p == '\t' || *L->p == '\r')
            L->p++;
        else if (L->end - L->p >= 3 && memcmp(L->p, "\xE3\x80\x80", 3) == 0)
            L->p += 3;
        else
            break;
    }
    t->b = L->p;
    if (L->p >= L->end) {
        t->kind = T_END;
        return;
    }
    static const struct { const char *s; int op; } two[] = {
        { "&&", OP_AND }, { "||", OP_OR }, { "==", OP_EQ }, { "!=", OP_NE }, { "<=", OP_LE }, { ">=", OP_GE },
    };
    for (size_t i = 0; i < sizeof two / sizeof two[0]; i++)
        if (L->end - L->p >= 2 && L->p[0] == two[i].s[0] && L->p[1] == two[i].s[1]) {
            t->kind = T_OP, t->op = two[i].op, L->p += 2, t->e = L->p;
            return;
        }
    char c = *L->p;
    if (c == '"') {
        const char *q = L->p + 1;
        while (q < L->end && *q != '"')
            q++;
        t->kind = T_STR, t->b = L->p + 1, t->e = q;
        L->p = q < L->end ? q + 1 : q;
        return;
    }
    if (c >= '0' && c <= '9') {
        int64_t v = 0;
        while (L->p < L->end && *L->p >= '0' && *L->p <= '9')
            v = v * 10 + (*L->p++ - '0');
        t->kind = T_NUM, t->num = (int32_t)v, t->e = L->p;
        return;
    }
    static const char single[] = "+-*/%<>()[]";
    const char *s = strchr(single, c);
    if (s) {
        static const int ops[] = { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_LT, OP_GT, '(', ')', '[', ']' };
        t->kind = T_OP, t->op = ops[s - single], L->p++, t->e = L->p;
        return;
    }
    while (L->p < L->end && !op_char(*L->p) && *L->p != ' ' && *L->p != '\t' &&
           !(L->end - L->p >= 3 && memcmp(L->p, "\xE3\x80\x80", 3) == 0))
        L->p++;
    if (L->p == t->b) { /* stray '=', '!', '&' or '|' */
        L->p++;
        L->bad = true;
    }
    t->kind = T_NAME, t->e = L->p;
}

static int32_t sequence(lexer *L);

static ft_var *lookup(lexer *L, const tok *name)
{
    char buf[128];
    size_t n = (size_t)(name->e - name->b);
    if (n >= sizeof buf)
        n = sizeof buf - 1;
    memcpy(buf, name->b, n);
    buf[n] = 0;
    ft_var *v = L->declare ? ft_vars_declare(L->vs, buf) : ft_vars_find(L->vs, buf);
    if (!v)
        var_error(L->vs, "undeclared variable %s", buf);
    return v;
}

/* kt_expr_operand */
static int32_t operand(lexer *L)
{
    tok t = L->cur;
    if (t.kind == T_OP && t.op == '(') {
        lex(L);
        int32_t v = sequence(L);
        if (L->cur.kind == T_OP && L->cur.op == ')')
            lex(L);
        else
            L->bad = true;
        return v;
    }
    if (t.kind == T_OP && t.op == OP_SUB) {
        lex(L);
        return -operand(L);
    }
    lex(L);
    if (t.kind == T_NUM)
        return t.num;
    if (t.kind == T_STR)
        return 0;
    if (t.kind != T_NAME) {
        L->bad = true;
        return 0;
    }
    ft_var *v = lookup(L, &t);
    if (L->cur.kind == T_OP && L->cur.op == '[') {
        /* the index may declare names and move the table: keep a position, not a pointer */
        ptrdiff_t pos = v ? v - L->vs->v : -1;
        lex(L);
        int32_t idx = sequence(L);
        if (L->cur.kind == T_OP && L->cur.op == ']')
            lex(L);
        else
            L->bad = true;
        if (pos < 0)
            return 0;
        v = &L->vs->v[pos];
        if (idx < 0 || (uint32_t)idx >= v->array_len) {
            var_error(L->vs, "%s[%d] out of range", v->name, idx);
            return 0;
        }
        return v->array[idx];
    }
    return v ? v->value : 0;
}

/* kt_expr_fold: strictly left to right */
static int32_t sequence(lexer *L)
{
    int32_t acc = operand(L);
    while (L->cur.kind == T_OP && L->cur.op >= OP_ADD && L->cur.op <= OP_OR) {
        int op = L->cur.op;
        lex(L);
        int32_t r = operand(L);
        switch (op) {
        case OP_ADD: acc += r; break;
        case OP_SUB: acc -= r; break;
        case OP_MUL: acc *= r; break;
        case OP_DIV: if (r) acc /= r; break;
        case OP_MOD: if (r) acc %= r; break;
        case OP_LE: acc = acc <= r; break;
        case OP_GE: acc = acc >= r; break;
        case OP_EQ: acc = acc == r; break;
        case OP_NE: acc = acc != r; break;
        case OP_LT: acc = acc < r; break;
        case OP_GT: acc = acc > r; break;
        case OP_AND: acc = acc != 0 && r != 0; break;
        case OP_OR: acc = acc != 0 || r != 0; break;
        }
    }
    return acc;
}

static int32_t eval_range(ft_vars *vs, const char *b, const char *e, bool declare, bool *bad)
{
    lexer L = { b, e, { 0 }, vs, declare, false };
    lex(&L);
    int32_t v = L.cur.kind == T_END ? 0 : sequence(&L);
    if (L.cur.kind != T_END)
        L.bad = true;
    if (bad)
        *bad = L.bad;
    return v;
}

int32_t ft_expr_eval(ft_vars *vs, const char *expr, bool declare)
{
    bool bad;
    int32_t v = eval_range(vs, expr, expr + strlen(expr), declare, &bad);
    if (bad)
        var_error(vs, "bad expression: %s", expr);
    return v;
}

/* Top-level assignment '=' (not part of == != <= >=), outside quotes and brackets. */
static const char *find_assign(const char *b, const char *e)
{
    int depth = 0;
    bool quote = false;
    for (const char *p = b; p < e; p++) {
        if (*p == '"')
            quote = !quote;
        else if (quote)
            continue;
        else if (*p == '[' || *p == '(')
            depth++;
        else if ((*p == ']' || *p == ')') && depth > 0)
            depth--;
        else if (*p == '=' && depth == 0) {
            if (p + 1 < e && p[1] == '=') {
                p++;
                continue;
            }
            if (p > b && (p[-1] == '!' || p[-1] == '<' || p[-1] == '>'))
                continue;
            return p;
        }
    }
    return NULL;
}

bool ft_expr_exec(ft_vars *vs, const char *stmt)
{
    const char *b = stmt, *e = stmt + strlen(stmt);
    const char *eq = find_assign(b, e);
    if (!eq) {
        bool bad;
        eval_range(vs, b, e, true, &bad);
        if (bad)
            var_error(vs, "bad statement: %s", stmt);
        return !bad;
    }

    /* lvalue: name or name[index] */
    const char *lb = b, *le = eq;
    while (lb < le && (*lb == ' ' || *lb == '\t'))
        lb++;
    while (le > lb && (le[-1] == ' ' || le[-1] == '\t'))
        le--;
    const char *br = memchr(lb, '[', (size_t)(le - lb));
    char name[128];
    size_t nlen = (size_t)((br ? br : le) - lb);
    while (nlen && (lb[nlen - 1] == ' ' || lb[nlen - 1] == '\t'))
        nlen--;
    if (nlen == 0 || nlen >= sizeof name) {
        var_error(vs, "bad assignment: %s", stmt);
        return false;
    }
    memcpy(name, lb, nlen);
    name[nlen] = 0;
    ft_var *v = ft_vars_declare(vs, name);
    if (!v)
        return false;
    int32_t idx = -1;
    if (br) {
        const char *close = le;
        while (close > br && close[-1] != ']')
            close--;
        bool bad;
        idx = eval_range(vs, br + 1, close > br ? close - 1 : le, false, &bad);
        if (bad || idx < 0 || !var_resize(v, (uint32_t)idx + 1)) {
            var_error(vs, "bad index: %s", stmt);
            return false;
        }
        v = ft_vars_find(vs, name); /* var_resize may not move v, but keep it simple */
    }

    const char *rb = eq + 1;
    while (rb < e && (*rb == ' ' || *rb == '\t'))
        rb++;
    if (*rb == '"') {
        const char *q = memchr(rb + 1, '"', (size_t)(e - rb - 1));
        size_t slen = (size_t)((q ? q : e) - rb - 1);
        char *s = malloc(slen + 1);
        if (!s)
            return false;
        memcpy(s, rb + 1, slen);
        s[slen] = 0;
        free(v->str);
        v->str = s;
        return true;
    }
    bool bad;
    int32_t val = eval_range(vs, rb, e, true, &bad);
    v = ft_vars_find(vs, name); /* declare mode may have grown the table */
    if (bad)
        var_error(vs, "bad expression: %s", stmt);
    if (idx >= 0)
        v->array[idx] = val;
    else
        v->value = val;
    return !bad;
}
