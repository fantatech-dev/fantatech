#include "core/ft_common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <io.h>
#include <wchar.h>
#include <windows.h>

/* UTF-8 to a malloc'd UTF-16 string, or NULL. */
static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) != n) {
        free(w);
        w = NULL;
    }
    return w;
}

FILE *ft_fopen(const char *path, const char *mode)
{
    wchar_t *wp = widen(path), *wm = widen(mode);
    FILE *f = wp && wm ? _wfopen(wp, wm) : NULL;
    free(wp);
    free(wm);
    return f;
}

int ft_remove(const char *path)
{
    wchar_t *w = widen(path);
    int r = w ? _wremove(w) : -1;
    free(w);
    return r;
}

int ft_rename(const char *from, const char *to)
{
    wchar_t *a = widen(from), *b = widen(to);
    int r = a && b ? _wrename(a, b) : -1;
    free(a);
    free(b);
    return r;
}

static int mkdir_utf8(const char *path)
{
    wchar_t *w = widen(path);
    int r = w ? _wmkdir(w) : -1;
    free(w);
    return r;
}
#define MKDIR(p) mkdir_utf8(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0777)

FILE *ft_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

int ft_remove(const char *path)
{
    return remove(path);
}

int ft_rename(const char *from, const char *to)
{
    return rename(from, to);
}
#endif

static char g_error[512];

const char *ft_get_error(void)
{
    return g_error;
}

void ft_set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof g_error, fmt, ap);
    va_end(ap);
}

bool ft_fseek64(FILE *f, uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(f, (long long)offset, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)offset, SEEK_SET) == 0;
#endif
}

bool ft_fsize64(FILE *f, uint64_t *size_out)
{
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0)
        return false;
    long long n = _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END) != 0)
        return false;
    long long n = (long long)ftello(f);
#endif
    if (n < 0)
        return false;
    *size_out = (uint64_t)n;
    return true;
}

uint8_t *ft_read_file(const char *path, size_t *len_out)
{
    FILE *f = ft_fopen(path, "rb");
    if (!f) {
        ft_set_error("cannot open %s", path);
        return NULL;
    }
    uint64_t size;
    if (!ft_fsize64(f, &size) || !ft_fseek64(f, 0) || size > SIZE_MAX - 1) {
        ft_set_error("cannot size %s", path);
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)size + 1);
    if (!buf) {
        ft_set_error("out of memory reading %s", path);
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        ft_set_error("short read on %s", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[size] = 0;
    *len_out = (size_t)size;
    return buf;
}

bool ft_write_file(const char *path, const void *data, size_t len)
{
    FILE *f = ft_fopen(path, "wb");
    if (!f) {
        ft_set_error("cannot create %s", path);
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok)
        ft_set_error("write failed on %s", path);
    return ok;
}

bool ft_mkdirs(const char *path)
{
    char buf[1024];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) {
        ft_set_error("bad directory path");
        return false;
    }
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] != '/' && buf[i] != '\\' && buf[i] != 0)
            continue;
        if (buf[i - 1] == ':' || buf[i - 1] == '/' || buf[i - 1] == '\\') /* "C:\", "//" */
            continue;
        char c = buf[i];
        buf[i] = 0;
        if (MKDIR(buf) != 0 && errno != EEXIST) {
            ft_set_error("cannot create directory %s", buf);
            return false;
        }
        buf[i] = c;
    }
    return true;
}

int ft_stricmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z')
            ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z')
            cb += 'a' - 'A';
        if (ca != cb || ca == 0)
            return ca - cb;
    }
}
