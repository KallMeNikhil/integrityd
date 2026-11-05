#include "integrityd.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static int needs_escape(unsigned char c, const char *extra)
{
    return c == '%' || c < 0x20 || c == 0x7f || (c && extra && strchr(extra, c));
}

void escape_append(Str *out, const char *s, const char *extra)
{
    static const char hx[] = "0123456789ABCDEF";
    const char *run = s;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!needs_escape(*p, extra)) continue;
        str_appendn(out, run, (size_t)((const char *)p - run));
        char e[3] = { '%', hx[*p >> 4], hx[*p & 15] };
        str_appendn(out, e, 3);
        run = (const char *)p + 1;
    }
    str_append(out, run);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int unescape(const char *s, Str *out)
{
    str_append(out, "");
    for (; *s; s++) {
        if (*s != '%') {
            str_appendc(out, *s);
            continue;
        }
        int hi = hexval(s[1]), lo = hi < 0 ? -1 : hexval(s[2]);
        if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return -1;
        str_appendc(out, (char)(hi * 16 + lo));
        s += 2;
    }
    return 0;
}

void detail_add(Str *d, const char *key, const char *val)
{
    if (d->len) str_appendc(d, ';');
    str_append(d, key);
    str_appendc(d, '=');
    escape_append(d, val, ESC_DETAIL);
}

void detail_addf(Str *d, const char *key, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    char *buf = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    detail_add(d, key, buf);
    free(buf);
}

int detail_get(const char *detail, const char *key, Str *out)
{
    size_t kl = strlen(key);
    const char *p = detail;
    while (p && *p) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            char *val = xstrndup(p + kl + 1, len - kl - 1);
            int rc = unescape(val, out);
            free(val);
            return rc == 0;
        }
        p = end ? end + 1 : NULL;
    }
    return 0;
}
