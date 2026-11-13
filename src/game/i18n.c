#include "game/i18n.h"

#include <stdlib.h>
#include <string.h>

uint32_t ft_i18n_hash(const char *utf8)
{
    uint32_t h = 2166136261u;
    for (const uint8_t *p = (const uint8_t *)utf8; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

void ft_i18n_key(const char *original, char out[9])
{
    snprintf(out, 9, "%08x", ft_i18n_hash(original));
}

static int cmp_entry(const void *a, const void *b)
{
    return strcmp(((const ft_i18n_entry *)a)->key, ((const ft_i18n_entry *)b)->key);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = 0;
    return s;
}

/* "\n" -> newline, "\\" -> backslash, in place. */
static void unescape(char *s)
{
    char *d = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') {
            *d++ = '\n';
            s++;
        } else if (s[0] == '\\' && s[1] == '\\') {
            *d++ = '\\';
            s++;
        } else {
            *d++ = *s;
        }
    }
    *d = 0;
}

bool ft_i18n_load(ft_i18n *t, const char *path)
{
    memset(t, 0, sizeof *t);
    size_t len;
    char *buf = (char *)ft_read_file(path, &len);
    if (!buf)
        return false;
    char *text = buf;
    if (len >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0) /* editors like to add a BOM */
        text += 3;
    uint32_t cap = 0;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        char *s = trim(line);
        if (!*s || *s == '#')
            continue;
        char *colon = strchr(s, ':');
        if (!colon)
            continue;
        *colon = 0;
        char *key = trim(s), *val = trim(colon + 1);
        if (strcmp(key, "@font") == 0) {
            snprintf(t->font, sizeof t->font, "%s", val);
            continue;
        }
        if (strcmp(key, "@name") == 0) {
            snprintf(t->name, sizeof t->name, "%s", val);
            continue;
        }
        if (!*key || !*val)
            continue;
        if (t->count == cap) {
            cap = cap ? cap * 2 : 1024;
            ft_i18n_entry *grown = realloc(t->e, sizeof *t->e * cap);
            if (!grown)
                break;
            t->e = grown;
        }
        unescape(val);
        t->e[t->count].key = malloc(strlen(key) + 1);
        t->e[t->count].text = malloc(strlen(val) + 1);
        if (!t->e[t->count].key || !t->e[t->count].text) {
            free(t->e[t->count].key);
            free(t->e[t->count].text);
            break;
        }
        strcpy(t->e[t->count].key, key);
        strcpy(t->e[t->count].text, val);
        t->count++;
    }
    free(buf);
    qsort(t->e, t->count, sizeof *t->e, cmp_entry);
    return true;
}

void ft_i18n_free(ft_i18n *t)
{
    for (uint32_t i = 0; i < t->count; i++) {
        free(t->e[i].key);
        free(t->e[i].text);
    }
    free(t->e);
    memset(t, 0, sizeof *t);
}

const char *ft_i18n_get(const ft_i18n *t, const char *key)
{
    if (!t || !t->count || !key || !*key)
        return NULL;
    ft_i18n_entry probe = { (char *)key, NULL };
    const ft_i18n_entry *e = bsearch(&probe, t->e, t->count, sizeof *t->e, cmp_entry);
    return e ? e->text : NULL;
}

const char *ft_i18n_text(const ft_i18n *t, const char *original)
{
    if (!t || !t->count || !original || !*original)
        return NULL;
    char key[9];
    ft_i18n_key(original, key);
    return ft_i18n_get(t, key);
}

const char *const ft_ui_strings[] = {
    "セーブ", "ロード", "空きスロット", "戻る", "削除", "はい", "いいえ",
    "このスロットに上書きしますか？", "このデータを読み込みますか？", "このデータを削除しますか？",
    "セーブしました", "削除しました", "セーブできませんでした", "読み込めませんでした",
    "前のセーブポイントでセーブしました",
    "クリック・Enter：決定　Tab：セーブ／ロード切替　Del：削除　Esc：戻る",
    "クリック・Enter：読み込む　Del：削除　Esc：戻る",
    "設定", "システム設定", "言語", "音量", "マスター", "音楽", "音声", "効果音", "画面", "フルスクリーン",
    "↑↓：項目　Enter：決定　←→：音量　Esc：戻る",
    "スクリーンショットを保存しました", "スクリーンショットを保存できませんでした",
};
const size_t ft_ui_string_count = sizeof ft_ui_strings / sizeof ft_ui_strings[0];
