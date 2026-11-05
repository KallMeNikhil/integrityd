#include "integrityd.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void oom(void)
{
    static const char msg[] = "integrityd: out of memory\n";
    (void)!write(2, msg, sizeof msg - 1);
    abort();
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) oom();
    return p;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void die(const char *fmt, ...)
{
    va_list ap;
    fputs("integrityd: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void warnmsg(const char *fmt, ...)
{
    va_list ap;
    fputs("integrityd: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void str_init(Str *s)
{
    s->p = NULL;
    s->len = s->cap = 0;
}

static void str_reserve(Str *s, size_t extra)
{
    if (s->len + extra + 1 <= s->cap) return;
    size_t cap = s->cap ? s->cap : 32;
    while (cap < s->len + extra + 1) cap *= 2;
    s->p = xrealloc(s->p, cap);
    s->cap = cap;
}

void str_appendn(Str *s, const char *t, size_t n)
{
    str_reserve(s, n);
    memcpy(s->p + s->len, t, n);
    s->len += n;
    s->p[s->len] = '\0';
}

void str_append(Str *s, const char *t) { str_appendn(s, t, strlen(t)); }
void str_appendc(Str *s, char c) { str_appendn(s, &c, 1); }

void str_appendf(Str *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    str_reserve(s, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(s->p + s->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    s->len += (size_t)n;
}

void str_clear(Str *s)
{
    s->len = 0;
    if (s->p) s->p[0] = '\0';
}

void str_free(Str *s)
{
    free(s->p);
    str_init(s);
}

char *str_steal(Str *s)
{
    char *p = s->p ? s->p : xstrdup("");
    str_init(s);
    return p;
}

void vec_init(Vec *v)
{
    v->items = NULL;
    v->n = v->cap = 0;
}

void vec_push(Vec *v, void *item)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->items = xrealloc(v->items, v->cap * sizeof *v->items);
    }
    v->items[v->n++] = item;
}

void vec_free(Vec *v)
{
    free(v->items);
    vec_init(v);
}

void vec_free_all(Vec *v, void (*free_item)(void *))
{
    for (size_t i = 0; i < v->n; i++) free_item(v->items[i]);
    vec_free(v);
}

static int64_t clock_ms(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t now_ms(void) { return clock_ms(CLOCK_MONOTONIC); }
int64_t wall_ms(void) { return clock_ms(CLOCK_REALTIME); }

void fmt_utc_ms(char buf[32], int64_t ms)
{
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, 32, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", (unsigned)(tm.tm_year + 1900) % 10000u,
             (unsigned)(tm.tm_mon + 1) % 100u, (unsigned)tm.tm_mday % 100u, (unsigned)tm.tm_hour % 100u,
             (unsigned)tm.tm_min % 100u, (unsigned)tm.tm_sec % 100u, (unsigned)(ms % 1000) % 1000u);
}

void fmt_utc(char buf[32], time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

char *path_join(const char *dir, const char *name)
{
    Str s;
    str_init(&s);
    str_append(&s, dir);
    if (s.len == 0 || s.p[s.len - 1] != '/') str_appendc(&s, '/');
    str_append(&s, name);
    return str_steal(&s);
}

int path_under(const char *path, const char *root)
{
    size_t n = strlen(root);
    if (n == 1 && root[0] == '/') return path[0] == '/';
    return strncmp(path, root, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

const char *path_base(const char *path)
{
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}

const char *path_rel(const char *path, const char *root)
{
    size_t n = strlen(root);
    if (n == 1 && root[0] == '/') return path + 1;
    if (path[n] == '/') return path + n + 1;
    return path + n;
}

int path_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int mkdir_p(const char *path, mode_t mode)
{
    char *p = xstrdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        if (mkdir(p, 0700) < 0 && errno != EEXIST) {
            free(p);
            return -1;
        }
        *s = '/';
    }
    int rc = mkdir(p, mode);
    free(p);
    if (rc < 0 && errno != EEXIST) return -1;
    return 0;
}

int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int read_file(const char *path, Str *out)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[65536];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd);
            errno = e;
            return -1;
        }
        if (r == 0) break;
        str_appendn(out, buf, (size_t)r);
    }
    close(fd);
    if (!out->p) str_append(out, "");
    return 0;
}

char *default_rundir(void)
{
    if (getuid() == 0) return xstrdup("/var/lib/integrityd");
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    return path_join(home, ".integrityd");
}
