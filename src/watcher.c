#include "integrityd.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define MASK_DIR (IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_MOVED_FROM | \
                  IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_DONT_FOLLOW | IN_ONLYDIR)
#define MASK_FILE (IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_DONT_FOLLOW)

int watcher_open(Watcher *w)
{
    memset(w, 0, sizeof *w);
    w->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (w->fd < 0) return -1;
    w->wd2dir = ht_new();
    w->dir2wd = ht_new();
    return 0;
}

void watcher_close(Watcher *w)
{
    if (w->fd >= 0) close(w->fd);
    w->fd = -1;
    ht_free(w->wd2dir, free);
    ht_free(w->dir2wd, NULL);
    w->wd2dir = w->dir2wd = NULL;
}

static void wd_key(char key[16], int wd) { snprintf(key, 16, "%d", wd); }

static int add_watch(Watcher *w, const char *path, int is_dir)
{
    int wd = inotify_add_watch(w->fd, path, is_dir ? MASK_DIR : MASK_FILE);
    if (wd < 0) {
        if (errno == ENOSPC) w->limit = 1;
        return -1;
    }
    char key[16];
    wd_key(key, wd);
    char *old = ht_get(w->wd2dir, key);
    if (old && strcmp(old, path) != 0 && (int)(intptr_t)ht_get(w->dir2wd, old) == wd) ht_del(w->dir2wd, old);
    free(ht_put(w->wd2dir, key, xstrdup(path)));
    ht_put(w->dir2wd, path, (void *)(intptr_t)wd);
    return 0;
}

static void forget(Watcher *w, const char *path, int rm)
{
    if (!ht_has(w->dir2wd, path)) return;
    int wd = (int)(intptr_t)ht_del(w->dir2wd, path);
    char key[16];
    wd_key(key, wd);
    char *p = ht_get(w->wd2dir, key);
    if (p && strcmp(p, path) == 0) free(ht_del(w->wd2dir, key));
    if (rm) inotify_rm_watch(w->fd, wd);
}

static int walk_add(Watcher *w, const ScanCtx *ctx, const char *root, dev_t root_dev, const char *dir,
                    int depth, HashTab *pending)
{
    int rc = add_watch(w, dir, 1) < 0 && w->limit ? -1 : 0;
    if (depth >= ctx->cfg->max_depth) return rc;
    DIR *d = opendir(dir);
    if (!d) return rc;
    Vec subdirs;
    vec_init(&subdirs);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char *child = path_join(dir, de->d_name);
        struct stat st;
        if (lstat(child, &st) < 0 || scan_excluded(ctx, root, child, S_ISDIR(st.st_mode))) {
            free(child);
            continue;
        }
        if (pending) ht_put(pending, child, NULL);
        if (S_ISDIR(st.st_mode) && !(ctx->cfg->one_filesystem && st.st_dev != root_dev))
            vec_push(&subdirs, child);
        else
            free(child);
    }
    closedir(d);
    for (size_t i = 0; i < subdirs.n; i++)
        if (walk_add(w, ctx, root, root_dev, subdirs.items[i], depth + 1, pending) < 0) rc = -1;
    vec_free_all(&subdirs, free);
    return rc;
}

int watcher_add_tree(Watcher *w, const ScanCtx *ctx, const char *root, const char *start, HashTab *pending)
{
    struct stat rst, st;
    if (lstat(root, &rst) < 0 || lstat(start, &st) < 0) return 0;
    if (scan_excluded(ctx, root, start, S_ISDIR(st.st_mode))) return 0;
    if (pending) ht_put(pending, start, NULL);
    if (!S_ISDIR(st.st_mode)) {
        if (strcmp(start, root) == 0 && S_ISREG(st.st_mode)) return add_watch(w, start, 0) < 0 && w->limit ? -1 : 0;
        return 0;
    }
    if (ctx->cfg->one_filesystem && st.st_dev != rst.st_dev) return 0;
    int depth = 0;
    if (strcmp(start, root) != 0) {
        depth = 1;
        for (const char *p = path_rel(start, root); *p; p++) depth += *p == '/';
    }
    return walk_add(w, ctx, root, rst.st_dev, start, depth, pending);
}

void watcher_remove_tree(Watcher *w, const char *dir)
{
    Vec doomed;
    vec_init(&doomed);
    HtIter it;
    const char *k;
    ht_iter_init(&it, w->dir2wd);
    while (ht_iter_next(&it, &k, NULL))
        if (path_under(k, dir)) vec_push(&doomed, xstrdup(k));
    for (size_t i = 0; i < doomed.n; i++) forget(w, doomed.items[i], 1);
    vec_free_all(&doomed, free);
}

int watcher_has(const Watcher *w, const char *path) { return w->dir2wd && ht_has(w->dir2wd, path); }
size_t watcher_count(const Watcher *w) { return w->dir2wd ? ht_count(w->dir2wd) : 0; }

static const char *root_of(const Vec *roots, const char *path)
{
    const char *best = NULL;
    for (size_t i = 0; i < roots->n; i++)
        if (path_under(path, roots->items[i]) && (!best || strlen(roots->items[i]) > strlen(best)))
            best = roots->items[i];
    return best;
}

int watcher_read(Watcher *w, const ScanCtx *ctx, const Vec *roots, HashTab *pending)
{
    _Alignas(struct inotify_event) char buf[64 * 1024];
    int count = 0;
    for (;;) {
        ssize_t n = read(w->fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) break;
            return -1;
        }
        if (n == 0) break;
        for (char *p = buf; p < buf + n;) {
            const struct inotify_event *ev = (const struct inotify_event *)p;
            p += sizeof *ev + ev->len;
            count++;
            if (ev->mask & IN_Q_OVERFLOW) {
                w->overflow = 1;
                continue;
            }
            char key[16];
            wd_key(key, ev->wd);
            const char *dir = ht_get(w->wd2dir, key);
            if (!dir) continue;
            if (ev->mask & IN_IGNORED) {
                char *d = xstrdup(dir);
                if ((int)(intptr_t)ht_get(w->dir2wd, d) == ev->wd) ht_del(w->dir2wd, d);
                free(ht_del(w->wd2dir, key));
                free(d);
                continue;
            }
            char *path = ev->len && ev->name[0] ? path_join(dir, ev->name) : xstrdup(dir);
            const char *root = root_of(roots, path);
            int is_dir = (ev->mask & IN_ISDIR) != 0;
            if (!root || scan_excluded(ctx, root, path, is_dir)) {
                free(path);
                continue;
            }
            if (is_dir && (ev->mask & (IN_CREATE | IN_MOVED_TO))) {
                if (watcher_add_tree(w, ctx, root, path, pending) < 0) w->limit = 1;
            }
            if (is_dir && (ev->mask & IN_MOVED_FROM)) watcher_remove_tree(w, path);
            if ((ev->mask & IN_MOVE_SELF) && strcmp(path, root) == 0) watcher_remove_tree(w, root);
            ht_put(pending, path, NULL);
            free(path);
        }
    }
    return count;
}
