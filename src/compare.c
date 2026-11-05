#include "integrityd.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct Update {
    char   *path;
    Record *rec;
} Update;

static const char *const ev_names[EV_COUNT] = { "CREATED", "MODIFIED", "DELETED", "PERMS", "TYPE_CHANGED",
                                                "MOVED", "REPLACED", "UNREADABLE", "UNSTABLE" };
static const int ev_sev[EV_COUNT] = { SEV_WARN, SEV_ALERT, SEV_ALERT, SEV_ALERT, SEV_ALERT,
                                      SEV_WARN, SEV_INFO, SEV_WARN, SEV_WARN };

const char *ev_name(int ev) { return ev >= 0 && ev < EV_COUNT ? ev_names[ev] : "?"; }
int ev_severity(int ev) { return ev >= 0 && ev < EV_COUNT ? ev_sev[ev] : SEV_INFO; }

static Change *push_change(Vec *out, int ev, const char *path, const Record *old, const Record *nw)
{
    Change *c = xcalloc(1, sizeof *c);
    c->ev = ev;
    c->path = xstrdup(path);
    c->old = old;
    c->nw = nw;
    vec_push(out, c);
    return c;
}

void change_free(void *p)
{
    Change *c = p;
    free(c->path);
    free(c);
}

void classify(const Record *old, const Record *nw, Vec *out)
{
    if (!old && !nw) return;
    if (!old) { push_change(out, EV_CREATED, nw->path, NULL, nw); return; }
    if (!nw) { push_change(out, EV_DELETED, old->path, old, NULL); return; }
    if (old->type != nw->type) {
        push_change(out, EV_TYPE_CHANGED, nw->path, old, nw);
        return;
    }
    int owner = old->uid != nw->uid || old->gid != nw->gid;
    int perms = owner || (old->type != 'l' && old->mode != nw->mode);
    int content = 0;
    if (old->type == 'f') {
        if (old->sha[0] && nw->sha[0]) {
            if (strcmp(old->sha, nw->sha) != 0) {
                Change *c = push_change(out, EV_MODIFIED, nw->path, old, nw);
                c->stealth = old->dev == nw->dev && old->ino == nw->ino && old->size == nw->size &&
                             old->mtime_ns == nw->mtime_ns;
                content = 1;
            }
        } else if (old->size != nw->size || old->mtime_ns != nw->mtime_ns ||
                   (!perms && old->ctime_ns != nw->ctime_ns)) {
            push_change(out, EV_MODIFIED, nw->path, old, nw)->basis_meta = 1;
            content = 1;
        }
        if (perms) push_change(out, EV_PERMS, nw->path, old, nw);
        if (!content && !perms && (old->dev != nw->dev || old->ino != nw->ino))
            push_change(out, EV_REPLACED, nw->path, old, nw);
        return;
    }
    if (old->type == 'l') {
        int tchg = (old->target && nw->target) ? strcmp(old->target, nw->target) != 0
                                               : (old->target != nw->target || strcmp(old->sha, nw->sha) != 0);
        if (tchg) push_change(out, EV_MODIFIED, nw->path, old, nw);
        if (owner) push_change(out, EV_PERMS, nw->path, old, nw);
        return;
    }
    if (perms) push_change(out, EV_PERMS, nw->path, old, nw);
}

void pair_moves(Vec *changes)
{
    HashTab *created = ht_new();
    char key[48];
    for (size_t i = 0; i < changes->n; i++) {
        Change *c = changes->items[i];
        if (c->ev != EV_CREATED || c->nw->type != 'f' || !c->nw->sha[0]) continue;
        snprintf(key, sizeof key, "%llu:%llu", (unsigned long long)c->nw->dev, (unsigned long long)c->nw->ino);
        ht_put(created, key, c);
    }
    if (ht_count(created)) {
        for (size_t i = 0; i < changes->n; i++) {
            Change *d = changes->items[i];
            if (d->ev != EV_DELETED || d->old->type != 'f' || !d->old->sha[0]) continue;
            snprintf(key, sizeof key, "%llu:%llu", (unsigned long long)d->old->dev,
                     (unsigned long long)d->old->ino);
            Change *c = ht_get(created, key);
            if (!c || c->ev != EV_CREATED || strcmp(c->nw->sha, d->old->sha) != 0) continue;
            c->ev = EV_MOVED;
            c->old = d->old;
            d->ev = -1;
            ht_del(created, key);
            if (c->old->mode != c->nw->mode || c->old->uid != c->nw->uid || c->old->gid != c->nw->gid)
                push_change(changes, EV_PERMS, c->nw->path, c->old, c->nw);
        }
    }
    ht_free(created, NULL);
    size_t j = 0;
    for (size_t i = 0; i < changes->n; i++) {
        Change *c = changes->items[i];
        if (c->ev < 0) change_free(c);
        else changes->items[j++] = c;
    }
    changes->n = j;
}

void engine_init(Engine *e, Snapshot *snap, const Config *cfg, const char *skip, emit_fn fn, void *ud)
{
    memset(e, 0, sizeof *e);
    e->snap = snap;
    e->cfg = cfg;
    e->ctx.cfg = cfg;
    e->ctx.skip = skip;
    e->emit = fn;
    e->ud = ud;
    e->unreadable_seen = ht_new();
    vec_init(&e->changes);
    vec_init(&e->newrecs);
    vec_init(&e->updates);
}

void engine_free(Engine *e)
{
    ht_free(e->unreadable_seen, NULL);
    ht_free(e->batch, NULL);
    vec_free_all(&e->changes, change_free);
    vec_free_all(&e->newrecs, record_free);
    vec_free(&e->updates);
}

void engine_reset_counters(Engine *e)
{
    e->n_files = e->n_changes = e->n_added = e->n_removed = e->n_changed = 0;
}

void engine_begin(Engine *e)
{
    if (!e->batch) e->batch = ht_new();
}

static const char *type_name(char t)
{
    return t == 'f' ? "f" : t == 'd' ? "d" : t == 'l' ? "l" : "o";
}

static void emit_change(Engine *e, const Change *c)
{
    const Record *o = c->old, *n = c->nw;
    Str d;
    str_init(&d);
    switch (c->ev) {
    case EV_CREATED:
        detail_add(&d, "type", type_name(n->type));
        detail_addf(&d, "size", "%lld", (long long)n->size);
        detail_add(&d, "sha", n->sha[0] ? n->sha : "-");
        detail_addf(&d, "mode", "%04o", (unsigned)n->mode);
        if (n->target) detail_add(&d, "target", n->target);
        break;
    case EV_MODIFIED:
        detail_add(&d, "old_sha", o->sha[0] ? o->sha : "-");
        detail_add(&d, "new_sha", n->sha[0] ? n->sha : "-");
        detail_addf(&d, "old_size", "%lld", (long long)o->size);
        detail_addf(&d, "new_size", "%lld", (long long)n->size);
        if (n->type == 'l') {
            detail_add(&d, "old_target", o->target ? o->target : "-");
            detail_add(&d, "new_target", n->target ? n->target : "-");
        }
        if (c->stealth) detail_add(&d, "stealth", "1");
        if (c->basis_meta) detail_add(&d, "basis", "meta");
        break;
    case EV_DELETED:
        detail_add(&d, "type", type_name(o->type));
        detail_add(&d, "old_sha", o->sha[0] ? o->sha : "-");
        detail_addf(&d, "old_size", "%lld", (long long)o->size);
        break;
    case EV_PERMS:
        detail_addf(&d, "old_mode", "%04o", (unsigned)o->mode);
        detail_addf(&d, "new_mode", "%04o", (unsigned)n->mode);
        detail_addf(&d, "old_uid", "%u", o->uid);
        detail_addf(&d, "new_uid", "%u", n->uid);
        detail_addf(&d, "old_gid", "%u", o->gid);
        detail_addf(&d, "new_gid", "%u", n->gid);
        break;
    case EV_TYPE_CHANGED:
        detail_add(&d, "old_type", type_name(o->type));
        detail_add(&d, "new_type", type_name(n->type));
        break;
    case EV_MOVED:
        detail_add(&d, "from", o->path);
        detail_add(&d, "sha", n->sha);
        detail_addf(&d, "size", "%lld", (long long)n->size);
        break;
    case EV_REPLACED:
        detail_addf(&d, "old_ino", "%llu", (unsigned long long)o->ino);
        detail_addf(&d, "new_ino", "%llu", (unsigned long long)n->ino);
        break;
    case EV_UNREADABLE:
        detail_addf(&d, "errno", "%d", c->err);
        detail_add(&d, "error", strerror(c->err));
        break;
    case EV_UNSTABLE:
        detail_add(&d, "retries", "3");
        break;
    }
    if (e->extra) detail_add(&d, e->extra, "1");
    e->emit(e->ud, ev_severity(c->ev), ev_name(c->ev), c->path, d.p);
    str_free(&d);
}

static int same_record(const Record *a, const Record *b)
{
    if (!record_tuple_eq(a, b) || strcmp(a->sha, b->sha) != 0) return 0;
    if (!a->target || !b->target) return a->target == b->target;
    return strcmp(a->target, b->target) == 0;
}

static void push_update(Engine *e, const char *path, Record *rec)
{
    Update *u = xmalloc(sizeof *u);
    u->path = xstrdup(path);
    u->rec = rec;
    vec_push(&e->updates, u);
}

static void note_unreadable(Engine *e, const char *path, int err)
{
    if (e->silent || ht_has(e->unreadable_seen, path)) return;
    ht_put(e->unreadable_seen, path, NULL);
    push_change(&e->changes, EV_UNREADABLE, path, NULL, NULL)->err = err;
}

void engine_item(Engine *e, const char *path, Record *nw, int hstat, int err)
{
    if (ht_has(e->batch, path)) {
        record_free(nw);
        return;
    }
    ht_put(e->batch, path, NULL);
    const Record *old = ht_get(e->snap->recs, path);
    if (nw) vec_push(&e->newrecs, nw);
    if (hstat == HASH_UNSTABLE) {
        if (!e->silent) push_change(&e->changes, EV_UNSTABLE, path, old, nw);
        return;
    }
    if (hstat == HASH_UNREADABLE) {
        note_unreadable(e, path, err);
        if (old && old->type == 'f') memcpy(nw->sha, old->sha, sizeof nw->sha);
    } else if (nw && nw->type == 'f' && nw->sha[0]) {
        ht_del(e->unreadable_seen, path);
    }
    classify(old, nw, &e->changes);
    if (old && nw && same_record(old, nw)) return;
    push_update(e, path, nw);
}

void engine_end(Engine *e)
{
    pair_moves(&e->changes);
    for (size_t i = 0; i < e->changes.n; i++) {
        Change *c = e->changes.items[i];
        if (c->ev == EV_CREATED) e->n_added++;
        else if (c->ev == EV_DELETED) e->n_removed++;
        else if (c->ev != EV_UNREADABLE && c->ev != EV_UNSTABLE) e->n_changed++;
        if (!e->silent) {
            emit_change(e, c);
            e->n_changes++;
        }
    }
    vec_free_all(&e->changes, change_free);
    for (size_t i = 0; i < e->updates.n; i++) {
        Update *u = e->updates.items[i];
        if (u->rec) snapshot_put(e->snap, u->rec);
        else snapshot_del(e->snap, u->path);
        free(u->path);
        free(u);
        e->dirty = 1;
    }
    vec_free(&e->updates);
    for (size_t i = 0; i < e->newrecs.n; i++) {
        Record *r = e->newrecs.items[i];
        if (ht_get(e->snap->recs, r->path) != r) record_free(r);
    }
    vec_free(&e->newrecs);
    ht_free(e->batch, NULL);
    e->batch = NULL;
}

int engine_hash(Engine *e, Record *nw, int *err)
{
    struct stat st;
    *err = 0;
    if (nw->type != 'f') return HASH_OK;
    int hs = hash_file(nw->path, e->cfg->hash_max_bytes, nw->sha, &st, err);
    if (hs == HASH_OK || hs == HASH_TOOBIG) record_set_stat(nw, &st);
    switch (hs) {
    case HASH_TOOBIG: return HASH_OK;
    case HASH_NOTREG: return HASH_UNSTABLE;
    case HASH_IOERR: return HASH_UNREADABLE;
    default: return hs;
    }
}

static void process(Engine *e, const char *path, Record *nw)
{
    int err = 0, hs = HASH_OK;
    if (nw && nw->type == 'f') {
        hs = engine_hash(e, nw, &err);
        if (hs == HASH_GONE) {
            record_free(nw);
            nw = NULL;
            hs = HASH_OK;
        }
    }
    engine_item(e, path, nw, hs, err);
}

static int under_any(const char *path, const Vec *dirs)
{
    for (size_t i = 0; i < dirs->n; i++)
        if (path_under(path, dirs->items[i]) && strcmp(path, dirs->items[i]) != 0) return 1;
    return 0;
}

static void scan_one(Engine *e, const char *root, const char *start, int deep)
{
    ScanResult res;
    scan_result_init(&res);
    if (scan_tree(&e->ctx, root, start, &res) < 0) {
        scan_result_free(&res);
        return;
    }
    HashTab *seen = ht_new();
    for (size_t i = 0; i < res.unreadable.n; i++)
        note_unreadable(e, res.unreadable.items[i], (int)(intptr_t)res.errs.items[i]);
    for (size_t i = 0; i < res.recs.n; i++) {
        Record *rec = res.recs.items[i];
        res.recs.items[i] = NULL;
        ht_put(seen, rec->path, NULL);
        e->n_files++;
        if (rec->type == 'd' && !under_any(rec->path, &res.unreadable)) {
            int unreadable_now = 0;
            for (size_t j = 0; j < res.unreadable.n; j++)
                if (strcmp(res.unreadable.items[j], rec->path) == 0) unreadable_now = 1;
            if (!unreadable_now) ht_del(e->unreadable_seen, rec->path);
        }
        const Record *old = ht_get(e->snap->recs, rec->path);
        if (!deep && old && record_tuple_eq(old, rec)) {
            record_free(rec);
            continue;
        }
        char *path = xstrdup(rec->path);
        process(e, path, rec);
        free(path);
    }
    Vec gone;
    vec_init(&gone);
    HtIter it;
    const char *k;
    ht_iter_init(&it, e->snap->recs);
    while (ht_iter_next(&it, &k, NULL))
        if (path_under(k, start) && !ht_has(seen, k) && !under_any(k, &res.unreadable)) vec_push(&gone, xstrdup(k));
    for (size_t i = 0; i < gone.n; i++) {
        const char *p = gone.items[i];
        const Record *old = ht_get(e->snap->recs, p);
        if (scan_excluded(&e->ctx, root, p, old->type == 'd')) {
            push_update(e, p, NULL);
            ht_put(e->batch, p, NULL);
        } else {
            engine_item(e, p, NULL, HASH_OK, 0);
        }
    }
    vec_free_all(&gone, free);
    ht_free(seen, NULL);
    scan_result_free(&res);
}

int engine_scan(Engine *e, const char *subtree, int deep)
{
    engine_begin(e);
    if (subtree) {
        const char *root = snapshot_root_of(e->snap, subtree);
        if (!root) {
            engine_end(e);
            return -1;
        }
        char *r = xstrdup(root);
        scan_one(e, r, subtree, deep);
        free(r);
    } else {
        Vec roots;
        vec_init(&roots);
        for (size_t i = 0; i < e->snap->roots.n; i++) vec_push(&roots, xstrdup(e->snap->roots.items[i]));
        for (size_t i = 0; i < roots.n; i++) scan_one(e, roots.items[i], roots.items[i], deep);
        vec_free_all(&roots, free);
    }
    engine_end(e);
    return 0;
}

static int excluded_any(Engine *e, const char *root, const char *path, int is_dir)
{
    if (scan_excluded(&e->ctx, root, path, is_dir)) return 1;
    char *p = xstrdup(path);
    int hit = 0;
    for (char *s = strrchr(p, '/'); s && !hit; s = strrchr(p, '/')) {
        *s = '\0';
        if (strlen(p) < strlen(root)) break;
        hit = scan_excluded(&e->ctx, root, p, 1);
    }
    free(p);
    return hit;
}

void engine_reconcile(Engine *e, HashTab *paths)
{
    Vec work;
    vec_init(&work);
    HashTab *inset = ht_new();
    Vec keys;
    vec_init(&keys);
    ht_keys(paths, &keys);
    for (size_t i = 0; i < keys.n; i++) {
        vec_push(&work, xstrdup(keys.items[i]));
        ht_put(inset, keys.items[i], NULL);
    }
    vec_free(&keys);
    engine_begin(e);
    for (size_t i = 0; i < work.n; i++) {
        const char *p = work.items[i];
        const char *root = snapshot_root_of(e->snap, p);
        if (!root) continue;
        Record *nw = NULL;
        int err = 0;
        if (record_from_path(p, &nw, &err) < 0 && err != ENOENT && err != ENOTDIR) continue;
        const Record *old = ht_get(e->snap->recs, p);
        int is_dir = nw ? nw->type == 'd' : (old && old->type == 'd');
        if (excluded_any(e, root, p, is_dir)) {
            record_free(nw);
            continue;
        }
        if (old && old->type == 'd' && (!nw || nw->type != 'd')) {
            Str prefix;
            str_init(&prefix);
            str_appendf(&prefix, "%s/", p);
            HtIter it;
            const char *k;
            ht_iter_init(&it, e->snap->recs);
            while (ht_iter_next(&it, &k, NULL)) {
                if (strncmp(k, prefix.p, prefix.len) == 0 && !ht_has(inset, k)) {
                    ht_put(inset, k, NULL);
                    vec_push(&work, xstrdup(k));
                }
            }
            str_free(&prefix);
        }
        e->n_files++;
        process(e, p, nw);
    }
    engine_end(e);
    vec_free_all(&work, free);
    ht_free(inset, NULL);
}

void engine_worker_result(Engine *e, const char *path, const char *sha, int64_t size, int64_t mtime_ns,
                          int64_t ctime_ns)
{
    Record *nw = NULL;
    int err;
    if (record_from_path(path, &nw, &err) < 0) return;
    if (nw->type != 'f' || nw->size != size || nw->mtime_ns != mtime_ns || nw->ctime_ns != ctime_ns ||
        strlen(sha) != 64) {
        record_free(nw);
        return;
    }
    memcpy(nw->sha, sha, 65);
    e->n_files++;
    engine_item(e, path, nw, HASH_OK, 0);
}
