#include "game/save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAVE_MAGIC "fantatech-save 1"

void ft_save_path(char *out, size_t cap, const char *dir, int slot, const char *ext)
{
    size_t n = strlen(dir);
    const char *sep = n && (dir[n - 1] == '/' || dir[n - 1] == '\\') ? "" : "/";
    snprintf(out, cap, "%s%s%02d.%s", dir, sep, slot, ext);
}

/* Copies `src` into a fixed field, cut to fit (a deliberate truncation, unlike snprintf "%s",
 * which gcc's -Wformat-truncation flags when the source may be longer). */
static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void ft_save_set(ft_save *s, const char *key, const char *value)
{
    if (s->kv_count == FT_SAVE_KV || !value || !value[0])
        return;
    copy_str(s->kv[s->kv_count].key, sizeof s->kv[0].key, key);
    copy_str(s->kv[s->kv_count].value, sizeof s->kv[0].value, value);
    s->kv_count++;
}

const char *ft_save_get(const ft_save *s, const char *key)
{
    for (int i = 0; i < s->kv_count; i++)
        if (strcmp(s->kv[i].key, key) == 0)
            return s->kv[i].value;
    return NULL;
}

bool ft_save_write(const char *path, const ft_save *s, const ft_vars *vars)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = ft_fopen(tmp, "wb");
    if (!f) {
        ft_set_error("cannot write %s", tmp);
        return false;
    }
    fprintf(f, "%s\ntime %lld\ntitle %s\n", SAVE_MAGIC, (long long)s->time, s->title);
    for (int i = 0; i < s->frames; i++)
        fprintf(f, "frame %s %u\n", s->frame[i].script, s->frame[i].line_no);
    for (int i = 0; i < s->kv_count; i++)
        fprintf(f, "set %s %s\n", s->kv[i].key, s->kv[i].value);
    for (uint32_t i = 0; i < vars->count; i++) {
        const ft_var *v = &vars->v[i];
        fprintf(f, "var %s %d\n", v->name, (int)v->value);
        if (v->str && !strpbrk(v->str, "\r\n"))
            fprintf(f, "str %s %s\n", v->name, v->str);
        if (v->array) {
            fprintf(f, "arr %s %u", v->name, v->array_len);
            for (uint32_t k = 0; k < v->array_len; k++)
                fprintf(f, " %d", (int)v->array[k]);
            fputc('\n', f);
        }
    }
    bool ok = !ferror(f);
    ok = fclose(f) == 0 && ok;
    if (ok) {
        ft_remove(path); /* _wrename doesn't replace */
        ok = ft_rename(tmp, path) == 0;
    }
    if (!ok) {
        ft_remove(tmp);
        ft_set_error("cannot write %s", path);
    }
    return ok;
}

/* Splits off the next space-separated word; returns the rest of the line. */
static char *word(char *s, char **w)
{
    while (*s == ' ')
        s++;
    *w = s;
    while (*s && *s != ' ')
        s++;
    if (*s)
        *s++ = 0;
    return s;
}

static void read_array(ft_vars *vs, char *name, char *rest)
{
    char *w;
    rest = word(rest, &w);
    uint32_t n = (uint32_t)strtoul(w, NULL, 10);
    ft_var *v = ft_vars_declare(vs, name);
    if (!v || n == 0 || n > 1u << 20)
        return;
    int32_t *a = calloc(n, sizeof *a);
    if (!a)
        return;
    for (uint32_t k = 0; k < n && *rest; k++) {
        rest = word(rest, &w);
        a[k] = (int32_t)strtol(w, NULL, 10);
    }
    free(v->array);
    v->array = a;
    v->array_len = n;
}

bool ft_save_read(const char *path, ft_save *s, bool vars)
{
    memset(s, 0, sizeof *s);
    ft_vars_init(&s->vars);
    FILE *f = ft_fopen(path, "rb");
    if (!f)
        return false;
    char line[8192];
    bool ok = fgets(line, sizeof line, f) && strncmp(line, SAVE_MAGIC, strlen(SAVE_MAGIC)) == 0;
    while (ok && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *key, *a, *rest = word(line, &key);
        if (strcmp(key, "time") == 0) {
            s->time = strtoll(rest, NULL, 10);
        } else if (strcmp(key, "title") == 0) {
            copy_str(s->title, sizeof s->title, rest);
        } else if (strcmp(key, "frame") == 0 && s->frames < FT_SAVE_FRAMES) {
            rest = word(rest, &a);
            copy_str(s->frame[s->frames].script, sizeof s->frame[0].script, a);
            s->frame[s->frames].line_no = (uint32_t)strtoul(rest, NULL, 10);
            s->frames++;
        } else if (strcmp(key, "set") == 0) {
            rest = word(rest, &a);
            ft_save_set(s, a, rest);
        } else if (!vars) {
            continue;
        } else if (strcmp(key, "var") == 0) {
            rest = word(rest, &a);
            ft_vars_set_int(&s->vars, a, (int32_t)strtol(rest, NULL, 10));
        } else if (strcmp(key, "str") == 0) {
            rest = word(rest, &a);
            ft_vars_set_str(&s->vars, a, rest);
        } else if (strcmp(key, "arr") == 0) {
            rest = word(rest, &a);
            read_array(&s->vars, a, rest);
        }
    }
    fclose(f);
    if (!ok || s->frames == 0) {
        ft_save_free(s);
        return false;
    }
    return true;
}

void ft_save_free(ft_save *s)
{
    ft_vars_free(&s->vars);
    ft_vars_init(&s->vars);
}
