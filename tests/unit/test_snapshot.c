#include "integrityd.h"
#include "check.h"

#include <stdlib.h>
#include <unistd.h>

static Record *mkrec(const char *path, char type, int64_t size)
{
    Record *r = record_new(path);
    r->type = type;
    r->dev = 2049;
    r->ino = (uint64_t)size + 100;
    r->mode = type == 'd' ? 0755 : 04755;
    r->uid = 1000;
    r->gid = 1000;
    r->nlink = 1;
    r->size = size;
    r->mtime_ns = 1700000000123456789LL;
    r->ctime_ns = 1700000000987654321LL;
    if (type == 'f') sha256_hex(path, strlen(path), r->sha);
    if (type == 'l') {
        r->target = xstrdup(size == 1 ? "-" : "target with\ttab");
        sha256_hex(r->target, strlen(r->target), r->sha);
    }
    return r;
}

static int same(const Snapshot *a, const Snapshot *b)
{
    if (ht_count(a->recs) != ht_count(b->recs) || a->roots.n != b->roots.n) return 0;
    for (size_t i = 0; i < a->roots.n; i++)
        if (strcmp(a->roots.items[i], b->roots.items[i])) return 0;
    HtIter it;
    const char *k;
    void *v;
    ht_iter_init(&it, a->recs);
    while (ht_iter_next(&it, &k, &v)) {
        const Record *x = v, *y = ht_get(b->recs, k);
        if (!y || !record_tuple_eq(x, y) || strcmp(x->sha, y->sha)) return 0;
        if ((x->target == NULL) != (y->target == NULL)) return 0;
        if (x->target && strcmp(x->target, y->target)) return 0;
    }
    return 1;
}

int main(void)
{
    char dir[] = "/tmp/itest_snap.XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char *path = path_join(dir, "snapshot.db");
    Str err, tmp;
    str_init(&err);
    str_init(&tmp);

    Snapshot a, b;
    snapshot_init(&a);
    snapshot_init(&b);
    CHECK(snapshot_load(&b, path, &err) == SNAP_MISSING);

    snapshot_add_root(&a, "/w/root with space");
    snapshot_add_root(&a, "/w/r%oot\nnl");
    snapshot_put(&a, mkrec("/w/root with space", 'd', 0));
    snapshot_put(&a, mkrec("/w/root with space/plain.txt", 'f', 10));
    snapshot_put(&a, mkrec("/w/root with space/tab\there", 'f', 11));
    snapshot_put(&a, mkrec("/w/root with space/new\nline", 'f', 12));
    snapshot_put(&a, mkrec("/w/root with space/pct%;=semi", 'f', 13));
    snapshot_put(&a, mkrec("/w/root with space/ctl\x01\x7f\xff", 'f', 14));
    snapshot_put(&a, mkrec("/w/root with space/link", 'l', 2));
    snapshot_put(&a, mkrec("/w/root with space/dashlink", 'l', 1));
    Record *empty = mkrec("/w/root with space/unhashed", 'f', 15);
    empty->sha[0] = '\0';
    snapshot_put(&a, empty);

    CHECK(snapshot_save(&a, path, &err) == 0);
    str_appendf(&tmp, "%s.tmp", path);
    CHECK(access(tmp.p, F_OK) != 0);
    CHECK(snapshot_load(&b, path, &err) == SNAP_OK);
    CHECK(same(&a, &b));
    CHECK(snapshot_root_of(&b, "/w/root with space/plain.txt") != NULL);
    CHECK(snapshot_root_of(&b, "/w/elsewhere") == NULL);

    Str content;
    str_init(&content);
    CHECK(read_file(path, &content) == 0);
    CHECK(strncmp(content.p, "#INTEGRITYD-SNAPSHOT v1\n", 24) == 0);
    CHECK(strstr(content.p, "#END 9 ") != NULL);
    CHECK(strstr(content.p, "tab%09here") != NULL && strstr(content.p, "new%0Aline") != NULL);

    char *mid = strstr(content.p, "plain.txt");
    *mid = 'P';
    FILE *f = fopen(path, "w");
    fputs(content.p, f);
    fclose(f);
    snapshot_free(&b);
    snapshot_init(&b);
    str_clear(&err);
    CHECK(snapshot_load(&b, path, &err) == SNAP_CORRUPT);
    CHECK(strstr(err.p, "checksum") != NULL);

    *mid = 'p';
    char *end = strstr(content.p, "#END");
    *end = '\0';
    f = fopen(path, "w");
    fputs(content.p, f);
    fclose(f);
    snapshot_free(&b);
    snapshot_init(&b);
    CHECK(snapshot_load(&b, path, &err) == SNAP_CORRUPT);

    f = fopen(path, "w");
    fputs("hello\n", f);
    fclose(f);
    snapshot_free(&b);
    snapshot_init(&b);
    CHECK(snapshot_load(&b, path, &err) == SNAP_CORRUPT);

    CHECK(snapshot_save(&a, path, &err) == 0);
    if (geteuid() != 0) {
        chmod(dir, 0500);
        snapshot_put(&a, mkrec("/w/root with space/extra", 'f', 99));
        str_clear(&err);
        CHECK(snapshot_save(&a, path, &err) < 0);
        chmod(dir, 0700);
        snapshot_free(&b);
        snapshot_init(&b);
        CHECK(snapshot_load(&b, path, &err) == SNAP_OK);
        CHECK(ht_count(b.recs) == 9);
    }

    size_t dropped = 0;
    snapshot_drop_under(&a, "/w/root with space", &dropped);
    CHECK(ht_count(a.recs) == 0 && dropped >= 9);
    CHECK(snapshot_remove_root(&a, "/w/root with space") == 0);
    CHECK(snapshot_remove_root(&a, "/w/nope") < 0);
    CHECK(a.roots.n == 1);

    Snapshot c;
    snapshot_init(&c);
    CHECK(snapshot_add_root(&c, "/x/a") == 0);
    CHECK(snapshot_add_root(&c, "/x/b") == 0);
    CHECK(snapshot_add_root(&c, "/x/a/deep") == 1);
    CHECK(snapshot_add_root(&c, "/x/ab") == 0);
    CHECK(c.roots.n == 3);
    CHECK(snapshot_add_root(&c, "/x") == 0);
    CHECK(c.roots.n == 1 && strcmp(c.roots.items[0], "/x") == 0);
    snapshot_free(&c);

    str_free(&content);
    snapshot_free(&a);
    snapshot_free(&b);
    unlink(path);
    rmdir(dir);
    free(path);
    str_free(&err);
    str_free(&tmp);
    return CHECK_DONE();
}
