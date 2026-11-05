#include "integrityd.h"
#include "check.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[64];

static void mk(const char *rel, const char *content)
{
    char *p = path_join(root, rel);
    if (content) {
        FILE *f = fopen(p, "w");
        fputs(content, f);
        fclose(f);
    } else {
        mkdir(p, 0755);
    }
    free(p);
}

static const Record *find(const ScanResult *r, const char *rel)
{
    char *p = path_join(root, rel);
    const Record *hit = NULL;
    for (size_t i = 0; i < r->recs.n; i++)
        if (strcmp(((Record *)r->recs.items[i])->path, p) == 0) hit = r->recs.items[i];
    free(p);
    return hit;
}

int main(void)
{
    strcpy(root, "/tmp/itest_scan.XXXXXX");
    CHECK(mkdtemp(root) != NULL);
    mk("a.txt", "hello");
    mk("sub", NULL);
    mk("sub/b.txt", "bee");
    mk("sub/deeper", NULL);
    mk("sub/deeper/c.txt", "sea");
    mk("x.swp", "swap");
    mk("cache", NULL);
    mk("cache/junk", "j");
    mk("sub/cache", "file named cache");
    mk("run", NULL);
    mk("run/change.log", "log");
    char *p;
    p = path_join(root, "loop1"); CHECK(symlink("loop2", p) == 0); free(p);
    p = path_join(root, "loop2"); CHECK(symlink("loop1", p) == 0); free(p);
    p = path_join(root, "dangling"); CHECK(symlink("/nonexistent/target", p) == 0); free(p);
    p = path_join(root, "fifo"); CHECK(mkfifo(p, 0600) == 0); free(p);

    Config cfg;
    config_init(&cfg);
    Str err;
    str_init(&err);
    config_set(&cfg, "exclude", "cache/", &err);
    char *rundir = path_join(root, "run");
    ScanCtx ctx = { &cfg, rundir };
    ScanResult r;
    scan_result_init(&r);
    CHECK(scan_tree(&ctx, root, root, &r) == 0);

    int sorted = 1;
    for (size_t i = 1; i < r.recs.n; i++)
        if (strcmp(((Record *)r.recs.items[i - 1])->path, ((Record *)r.recs.items[i])->path) >= 0) sorted = 0;
    CHECK(sorted);

    const Record *rec = find(&r, "");
    CHECK(rec == NULL);
    CHECK(strcmp(((Record *)r.recs.items[0])->path, root) == 0 && ((Record *)r.recs.items[0])->type == 'd');
    CHECK((rec = find(&r, "a.txt")) && rec->type == 'f' && rec->size == 5 && rec->sha[0] == '\0');
    CHECK((rec = find(&r, "sub/deeper/c.txt")) && rec->type == 'f');
    CHECK((rec = find(&r, "loop1")) && rec->type == 'l' && strcmp(rec->target, "loop2") == 0 && rec->sha[0]);
    CHECK((rec = find(&r, "dangling")) && rec->type == 'l' && strcmp(rec->target, "/nonexistent/target") == 0);
    CHECK((rec = find(&r, "fifo")) && rec->type == 'o' && rec->sha[0] == '\0');
    CHECK(find(&r, "x.swp") == NULL);
    CHECK(find(&r, "cache") == NULL && find(&r, "cache/junk") == NULL);
    CHECK(find(&r, "sub/cache") != NULL);
    CHECK(find(&r, "run") == NULL && find(&r, "run/change.log") == NULL);
    CHECK(r.unreadable.n == 0);
    size_t total = r.recs.n;
    CHECK(total == 11);

    Str line;
    str_init(&line);
    for (size_t i = 0; i < r.recs.n; i++) {
        Record *a = r.recs.items[i], *b = NULL;
        str_clear(&line);
        record_format(&line, a);
        CHECK(record_parse(line.p, &b) == 0);
        CHECK(b && strcmp(a->path, b->path) == 0 && record_tuple_eq(a, b) && strcmp(a->sha, b->sha) == 0);
        CHECK(b && ((!a->target && !b->target) || (a->target && b->target && !strcmp(a->target, b->target))));
        record_free(b);
    }
    Record *bad = NULL;
    CHECK(record_parse("f\trelative\t1\t2\t0644\t0\t0\t1\t0\t0\t0\t-\t-", &bad) < 0);
    CHECK(record_parse("q\t/x\t1\t2\t0644\t0\t0\t1\t0\t0\t0\t-\t-", &bad) < 0);
    CHECK(record_parse("f\t/x\t1\t2\t0644\t0\t0\t1\t0\t0\t0\tzz\t-", &bad) < 0);
    CHECK(record_parse("f\t/x\t1\t2\t0644", &bad) < 0);
    str_free(&line);
    scan_result_free(&r);

    cfg.max_depth = 2;
    scan_result_init(&r);
    CHECK(scan_tree(&ctx, root, root, &r) == 0);
    CHECK(find(&r, "sub/deeper") != NULL && find(&r, "sub/deeper/c.txt") == NULL);
    scan_result_free(&r);
    cfg.max_depth = 64;

    char *sub = path_join(root, "sub");
    scan_result_init(&r);
    CHECK(scan_tree(&ctx, root, sub, &r) == 0);
    CHECK(r.recs.n == 5 && find(&r, "a.txt") == NULL);
    scan_result_free(&r);

    if (geteuid() != 0) {
        chmod(sub, 0);
        scan_result_init(&r);
        CHECK(scan_tree(&ctx, root, root, &r) == 0);
        CHECK(r.unreadable.n == 1 && strcmp(r.unreadable.items[0], sub) == 0);
        CHECK(find(&r, "sub") != NULL && find(&r, "sub/b.txt") == NULL);
        scan_result_free(&r);
        chmod(sub, 0755);
    }

    scan_result_init(&r);
    CHECK(scan_tree(&ctx, "/nonexistent/root", "/nonexistent/root", &r) == 0 && r.root_missing);
    scan_result_free(&r);

    free(sub);
    free(rundir);
    str_free(&err);
    config_free(&cfg);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    CHECK(system(cmd) == 0);
    return CHECK_DONE();
}
