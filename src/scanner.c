#include "integrityd.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

Record *record_new(const char *path)
{
    Record *r = xcalloc(1, sizeof *r);
    r->path = xstrdup(path);
    return r;
}

Record *record_dup(const Record *r)
{
    Record *d = xmalloc(sizeof *d);
    *d = *r;
    d->path = xstrdup(r->path);
    d->target = r->target ? xstrdup(r->target) : NULL;
    return d;
}

void record_free(void *p)
{
    Record *r = p;
    if (!r) return;
    free(r->path);
    free(r->target);
    free(r);
}

void record_set_stat(Record *r, const struct stat *st)
{
    r->type = S_ISREG(st->st_mode) ? 'f' : S_ISDIR(st->st_mode) ? 'd' : S_ISLNK(st->st_mode) ? 'l' : 'o';
    r->dev = (uint64_t)st->st_dev;
    r->ino = (uint64_t)st->st_ino;
    r->nlink = (uint64_t)st->st_nlink;
    r->mode = (uint32_t)(st->st_mode & 07777);
    r->uid = (uint32_t)st->st_uid;
    r->gid = (uint32_t)st->st_gid;
    r->size = (int64_t)st->st_size;
    r->mtime_ns = (int64_t)st->st_mtim.tv_sec * 1000000000 + st->st_mtim.tv_nsec;
    r->ctime_ns = (int64_t)st->st_ctim.tv_sec * 1000000000 + st->st_ctim.tv_nsec;
}

static char *read_link(const char *path, size_t hint)
{
    size_t cap = hint ? hint + 1 : 64;
    for (;;) {
        char *buf = xmalloc(cap);
        ssize_t n = readlink(path, buf, cap);
        if (n < 0) {
            free(buf);
            return NULL;
        }
        if ((size_t)n < cap) {
            buf[n] = '\0';
            return buf;
        }
        free(buf);
        cap *= 2;
    }
}

static Record *record_from_stat(const char *path, const struct stat *st)
{
    Record *r = record_new(path);
    record_set_stat(r, st);
    if (r->type == 'l') {
        r->target = read_link(path, (size_t)st->st_size);
        if (r->target) sha256_hex(r->target, strlen(r->target), r->sha);
    }
    return r;
}

int record_from_path(const char *path, Record **out, int *err)
{
    struct stat st;
    *out = NULL;
    if (lstat(path, &st) < 0) {
        if (err) *err = errno;
        return -1;
    }
    *out = record_from_stat(path, &st);
    return 0;
}

int record_cmp(const void *a, const void *b)
{
    const Record *x = *(Record *const *)a, *y = *(Record *const *)b;
    return strcmp(x->path, y->path);
}

int record_tuple_eq(const Record *a, const Record *b)
{
    return a->type == b->type && a->dev == b->dev && a->ino == b->ino && a->size == b->size &&
           a->mtime_ns == b->mtime_ns && a->ctime_ns == b->ctime_ns && a->mode == b->mode &&
           a->uid == b->uid && a->gid == b->gid && a->nlink == b->nlink;
}

void record_format(Str *out, const Record *r)
{
    str_appendc(out, r->type);
    str_appendc(out, '\t');
    escape_append(out, r->path, ESC_PATH);
    str_appendf(out, "\t%llu\t%llu\t%04o\t%u\t%u\t%llu\t%lld\t%lld\t%lld\t%s\t",
                (unsigned long long)r->dev, (unsigned long long)r->ino, (unsigned)r->mode, r->uid, r->gid,
                (unsigned long long)r->nlink, (long long)r->size, (long long)r->mtime_ns,
                (long long)r->ctime_ns, r->sha[0] ? r->sha : "-");
    if (!r->target) str_appendc(out, '-');
    else if (strcmp(r->target, "-") == 0) str_append(out, "%2D");
    else escape_append(out, r->target, ESC_PATH);
}

static int parse_u64(const char *s, uint64_t *v)
{
    char *end;
    if (!*s || *s == '-') return -1;
    errno = 0;
    unsigned long long x = strtoull(s, &end, 10);
    if (errno || *end) return -1;
    *v = x;
    return 0;
}

static int parse_i64(const char *s, int64_t *v)
{
    char *end;
    if (!*s) return -1;
    errno = 0;
    long long x = strtoll(s, &end, 10);
    if (errno || *end) return -1;
    *v = x;
    return 0;
}

int record_parse(const char *line, Record **out)
{
    char *copy = xstrdup(line), *f[13];
    int n = 0;
    char *p = copy;
    *out = NULL;
    while (n < 13) {
        f[n++] = p;
        char *tab = strchr(p, '\t');
        if (!tab) break;
        *tab = '\0';
        p = tab + 1;
    }
    Record *r = NULL;
    Str path, tgt;
    str_init(&path);
    str_init(&tgt);
    uint64_t mode, uid, gid;
    char *end;
    if (n != 13 || strchr(f[12], '\t') || strlen(f[0]) != 1 || !strchr("fdlo", f[0][0])) goto bad;
    if (unescape(f[1], &path) < 0 || path.p[0] != '/') goto bad;
    r = record_new(path.p);
    r->type = f[0][0];
    if (parse_u64(f[2], &r->dev) || parse_u64(f[3], &r->ino) || parse_u64(f[5], &uid) ||
        parse_u64(f[6], &gid) || parse_u64(f[7], &r->nlink) || parse_i64(f[8], &r->size) ||
        parse_i64(f[9], &r->mtime_ns) || parse_i64(f[10], &r->ctime_ns))
        goto bad;
    errno = 0;
    mode = strtoull(f[4], &end, 8);
    if (errno || *end || !*f[4] || mode > 07777 || uid > UINT32_MAX || gid > UINT32_MAX) goto bad;
    r->mode = (uint32_t)mode;
    r->uid = (uint32_t)uid;
    r->gid = (uint32_t)gid;
    if (strcmp(f[11], "-") != 0) {
        if (strlen(f[11]) != 64 || strspn(f[11], "0123456789abcdef") != 64) goto bad;
        memcpy(r->sha, f[11], 65);
    }
    if (strcmp(f[12], "-") != 0) {
        if (unescape(f[12], &tgt) < 0) goto bad;
        r->target = str_steal(&tgt);
    }
    str_free(&path);
    free(copy);
    *out = r;
    return 0;
bad:
    record_free(r);
    str_free(&path);
    str_free(&tgt);
    free(copy);
    return -1;
}

void scan_result_init(ScanResult *r)
{
    vec_init(&r->recs);
    vec_init(&r->unreadable);
    vec_init(&r->errs);
    r->root_missing = 0;
}

void scan_result_free(ScanResult *r)
{
    vec_free_all(&r->recs, record_free);
    vec_free_all(&r->unreadable, free);
    vec_free(&r->errs);
}

int scan_excluded(const ScanCtx *ctx, const char *root, const char *path, int is_dir)
{
    if (ctx->skip && path_under(path, ctx->skip)) return 1;
    if (strcmp(path, root) == 0) return 0;
    return config_excluded(ctx->cfg, path_rel(path, root), is_dir);
}

static void mark_unreadable(ScanResult *out, const char *dir, int err)
{
    if (out->unreadable.n && strcmp(out->unreadable.items[out->unreadable.n - 1], dir) == 0) return;
    vec_push(&out->unreadable, xstrdup(dir));
    vec_push(&out->errs, (void *)(intptr_t)err);
}

typedef struct {
    const ScanCtx *ctx;
    const char    *root;
    dev_t          root_dev;
    ScanResult    *out;
} Walk;

static void walk(Walk *w, const char *dir, int depth)
{
    if (depth >= w->ctx->cfg->max_depth) return;
    DIR *d = opendir(dir);
    if (!d) {
        mark_unreadable(w->out, dir, errno);
        return;
    }
    Vec subdirs;
    vec_init(&subdirs);
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(d);
        if (!de) {
            if (errno) mark_unreadable(w->out, dir, errno);
            break;
        }
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char *child = path_join(dir, de->d_name);
        struct stat st;
        if (lstat(child, &st) < 0) {
            if (errno != ENOENT) mark_unreadable(w->out, dir, errno);
            free(child);
            continue;
        }
        if (scan_excluded(w->ctx, w->root, child, S_ISDIR(st.st_mode))) {
            free(child);
            continue;
        }
        vec_push(&w->out->recs, record_from_stat(child, &st));
        if (S_ISDIR(st.st_mode) && !(w->ctx->cfg->one_filesystem && st.st_dev != w->root_dev))
            vec_push(&subdirs, child);
        else
            free(child);
    }
    closedir(d);
    for (size_t i = 0; i < subdirs.n; i++) walk(w, subdirs.items[i], depth + 1);
    vec_free_all(&subdirs, free);
}

int scan_tree(const ScanCtx *ctx, const char *root, const char *start, ScanResult *out)
{
    struct stat rst, st;
    if (lstat(root, &rst) < 0) {
        if (errno != ENOENT && errno != ENOTDIR) return -1;
        out->root_missing = 1;
        return 0;
    }
    if (lstat(start, &st) < 0) {
        if (errno == ENOENT || errno == ENOTDIR) return 0;
        return -1;
    }
    if (scan_excluded(ctx, root, start, S_ISDIR(st.st_mode))) return 0;
    vec_push(&out->recs, record_from_stat(start, &st));
    if (S_ISDIR(st.st_mode) && !(ctx->cfg->one_filesystem && st.st_dev != rst.st_dev)) {
        int depth = 0;
        for (const char *p = path_rel(start, root); *p; p++)
            if (*p == '/') depth++;
        if (strcmp(start, root) != 0) depth++;
        Walk w = { ctx, root, rst.st_dev, out };
        walk(&w, start, depth);
    }
    if (out->recs.n > 1) qsort(out->recs.items, out->recs.n, sizeof *out->recs.items, record_cmp);
    return 0;
}
