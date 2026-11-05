#include "integrityd.h"
#include "check.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static Record base(void)
{
    Record r;
    memset(&r, 0, sizeof r);
    r.type = 'f';
    r.path = "/r/file";
    r.dev = 1;
    r.ino = 100;
    r.nlink = 1;
    r.mode = 0644;
    r.uid = 1000;
    r.gid = 1000;
    r.size = 10;
    r.mtime_ns = 1000;
    r.ctime_ns = 1000;
    strcpy(r.sha, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    return r;
}

#define SHA_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static char result[256];
static const char *run(const Record *o, const Record *n)
{
    Vec v;
    vec_init(&v);
    classify(o, n, &v);
    result[0] = '\0';
    for (size_t i = 0; i < v.n; i++) {
        Change *c = v.items[i];
        if (i) strcat(result, " ");
        strcat(result, ev_name(c->ev));
        if (c->stealth) strcat(result, "+stealth");
        if (c->basis_meta) strcat(result, "+meta");
    }
    vec_free_all(&v, change_free);
    return result;
}

static Vec events;
static char root[64];

static void capture(void *ud, int sev, const char *event, const char *path, const char *detail)
{
    (void)ud;
    (void)sev;
    Str s;
    str_init(&s);
    str_appendf(&s, "%s %s %s", event, path ? path_rel(path, root) : "-", detail ? detail : "");
    vec_push(&events, str_steal(&s));
}

static int has_event(const char *prefix, const char *detail_part)
{
    for (size_t i = 0; i < events.n; i++)
        if (strncmp(events.items[i], prefix, strlen(prefix)) == 0 &&
            (!detail_part || strstr(events.items[i], detail_part)))
            return 1;
    return 0;
}

static void clear_events(void)
{
    vec_free_all(&events, free);
    vec_init(&events);
}

static void writef(const char *rel, const char *content)
{
    char *p = path_join(root, rel);
    FILE *f = fopen(p, "w");
    fputs(content, f);
    fclose(f);
    free(p);
}

int main(void)
{
    Record o = base(), n;

    CHECK_STREQ(run(NULL, NULL), "");
    n = base();
    CHECK_STREQ(run(NULL, &n), "CREATED");
    CHECK_STREQ(run(&o, NULL), "DELETED");
    n = base(); n.type = 'd';
    CHECK_STREQ(run(&o, &n), "TYPE_CHANGED");
    n = base(); n.type = 'l';
    CHECK_STREQ(run(&o, &n), "TYPE_CHANGED");

    n = base(); strcpy(n.sha, SHA_B); n.size = 11; n.mtime_ns = 2000; n.ctime_ns = 2000;
    CHECK_STREQ(run(&o, &n), "MODIFIED");
    n = base(); strcpy(n.sha, SHA_B); n.ctime_ns = 5000;
    CHECK_STREQ(run(&o, &n), "MODIFIED+stealth");
    n = base(); strcpy(n.sha, SHA_B); n.ino = 101;
    CHECK_STREQ(run(&o, &n), "MODIFIED");
    n = base(); n.sha[0] = '\0'; n.size = 12;
    CHECK_STREQ(run(&o, &n), "MODIFIED+meta");
    n = base(); n.sha[0] = '\0'; n.ctime_ns = 3000;
    CHECK_STREQ(run(&o, &n), "MODIFIED+meta");
    n = base(); n.sha[0] = '\0'; n.ctime_ns = 3000; n.mode = 0600;
    CHECK_STREQ(run(&o, &n), "PERMS");
    n = base(); n.sha[0] = '\0';
    CHECK_STREQ(run(&o, &n), "");
    n = base(); n.mode = 0600; n.ctime_ns = 2000;
    CHECK_STREQ(run(&o, &n), "PERMS");
    n = base(); n.uid = 0;
    CHECK_STREQ(run(&o, &n), "PERMS");
    n = base(); n.gid = 5;
    CHECK_STREQ(run(&o, &n), "PERMS");
    n = base(); n.mode = 04755;
    CHECK_STREQ(run(&o, &n), "PERMS");
    n = base(); n.mode = 0600; strcpy(n.sha, SHA_B); n.size = 3;
    CHECK_STREQ(run(&o, &n), "MODIFIED PERMS");
    n = base(); n.ino = 555; n.ctime_ns = 9000;
    CHECK_STREQ(run(&o, &n), "REPLACED");
    n = base(); n.dev = 2;
    CHECK_STREQ(run(&o, &n), "REPLACED");
    n = base(); n.mtime_ns = 7000; n.ctime_ns = 7000; n.nlink = 2;
    CHECK_STREQ(run(&o, &n), "");

    Record lo = base(), ln;
    lo.type = 'l'; lo.mode = 0777; lo.target = "a";
    ln = lo; ln.target = "b"; strcpy(ln.sha, SHA_B);
    CHECK_STREQ(run(&lo, &ln), "MODIFIED");
    ln = lo; ln.uid = 0;
    CHECK_STREQ(run(&lo, &ln), "PERMS");
    ln = lo; ln.mode = 0755;
    CHECK_STREQ(run(&lo, &ln), "");
    Record dold = base(), dnew;
    dold.type = 'd'; dold.sha[0] = '\0';
    dnew = dold; dnew.mode = 0700;
    CHECK_STREQ(run(&dold, &dnew), "PERMS");
    dnew = dold; dnew.size = 4096; dnew.mtime_ns = 9;
    CHECK_STREQ(run(&dold, &dnew), "");
    dold.type = 'o'; dnew = dold; dnew.gid = 7;
    CHECK_STREQ(run(&dold, &dnew), "PERMS");

    {
        Record a = base(), b = base(), c = base();
        a.path = "/r/old";
        b.path = "/r/new";
        c.path = "/r/other"; c.ino = 100; strcpy(c.sha, SHA_B);
        Vec v;
        vec_init(&v);
        classify(&a, NULL, &v);
        classify(NULL, &b, &v);
        pair_moves(&v);
        CHECK(v.n == 1 && ((Change *)v.items[0])->ev == EV_MOVED);
        CHECK(v.n == 1 && ((Change *)v.items[0])->old == &a);
        vec_free_all(&v, change_free);
        vec_init(&v);
        classify(&a, NULL, &v);
        classify(NULL, &c, &v);
        pair_moves(&v);
        CHECK(v.n == 2 && ((Change *)v.items[0])->ev == EV_DELETED && ((Change *)v.items[1])->ev == EV_CREATED);
        vec_free_all(&v, change_free);
        vec_init(&v);
        b.mode = 0600;
        classify(&a, NULL, &v);
        classify(NULL, &b, &v);
        pair_moves(&v);
        CHECK(v.n == 2 && ((Change *)v.items[0])->ev == EV_MOVED && ((Change *)v.items[1])->ev == EV_PERMS);
        vec_free_all(&v, change_free);
    }

    strcpy(root, "/tmp/itest_cmp.XXXXXX");
    CHECK(mkdtemp(root) != NULL);
    char *sub = path_join(root, "sub");
    mkdir(sub, 0755);
    writef("a.txt", "hello world\n");
    writef("sub/b.txt", "bee\n");
    writef("sub/c.txt", "sea\n");
    Config cfg;
    config_init(&cfg);
    Snapshot snap;
    snapshot_init(&snap);
    snapshot_add_root(&snap, root);
    Engine e;
    vec_init(&events);
    engine_init(&e, &snap, &cfg, NULL, capture, NULL);
    e.silent = 1;
    engine_scan(&e, NULL, 1);
    CHECK(events.n == 0 && ht_count(snap.recs) == 5 && e.dirty);
    e.silent = 0;
    engine_reset_counters(&e);

    engine_scan(&e, NULL, 0);
    CHECK(events.n == 0);

    char *a = path_join(root, "a.txt");
    struct stat st;
    stat(a, &st);
    writef("a.txt", "HELLO WORLD\n");
    struct timespec ts[2] = { st.st_atim, st.st_mtim };
    utimensat(AT_FDCWD, a, ts, 0);
    engine_scan(&e, NULL, 0);
    CHECK(events.n == 1 && has_event("MODIFIED a.txt", "stealth=1"));
    clear_events();
    engine_scan(&e, NULL, 0);
    CHECK(events.n == 0);

    engine_scan(&e, NULL, 1);
    CHECK(events.n == 0);

    chmod(a, 0600);
    engine_scan(&e, NULL, 0);
    CHECK(events.n == 1 && has_event("PERMS a.txt", "new_mode=0600"));
    clear_events();

    if (geteuid() != 0) {
        chmod(a, 0);
        engine_scan(&e, NULL, 1);
        CHECK(events.n == 2 && has_event("PERMS a.txt", NULL) && has_event("UNREADABLE a.txt", "errno=13"));
        clear_events();
        engine_scan(&e, NULL, 1);
        CHECK(events.n == 0);
        const Record *r = ht_get(snap.recs, a);
        CHECK(r && r->sha[0]);
        chmod(a, 0644);
        engine_scan(&e, NULL, 1);
        CHECK(events.n == 1 && has_event("PERMS a.txt", NULL));
        clear_events();
        chmod(a, 0);
        engine_scan(&e, NULL, 1);
        CHECK(has_event("UNREADABLE a.txt", NULL));
        clear_events();
        chmod(a, 0644);
        engine_scan(&e, NULL, 1);
        clear_events();

        chmod(sub, 0);
        engine_scan(&e, NULL, 0);
        CHECK(has_event("UNREADABLE sub", NULL) && !has_event("DELETED", NULL));
        clear_events();
        chmod(sub, 0755);
        engine_scan(&e, NULL, 0);
        CHECK(!has_event("DELETED", NULL) && !has_event("CREATED", NULL));
        clear_events();
    }

    char *b = path_join(root, "sub/b.txt"), *c = path_join(root, "sub/c.txt");
    unlink(b);
    unlink(c);
    rmdir(sub);
    HashTab *pending = ht_new();
    ht_put(pending, sub, NULL);
    engine_reconcile(&e, pending);
    CHECK(events.n == 3 && has_event("DELETED sub ", NULL) && has_event("DELETED sub/b.txt", NULL) &&
          has_event("DELETED sub/c.txt", NULL));
    clear_events();
    ht_free(pending, NULL);

    unlink(a);
    mkdir(a, 0755);
    engine_scan(&e, NULL, 0);
    CHECK(events.n == 1 && has_event("TYPE_CHANGED a.txt", "new_type=d"));
    clear_events();
    rmdir(a);
    engine_scan(&e, NULL, 0);
    CHECK(events.n == 1 && has_event("DELETED a.txt", NULL));
    clear_events();
    CHECK(ht_count(snap.recs) == 1);

    writef("z", "z");
    e.extra = "offline";
    engine_scan(&e, NULL, 0);
    CHECK(has_event("CREATED z", "offline=1"));
    clear_events();

    char *z = path_join(root, "z");
    unlink(z);
    rmdir(root);
    free(z); free(a); free(b); free(c); free(sub);
    engine_free(&e);
    snapshot_free(&snap);
    config_free(&cfg);
    vec_free_all(&events, free);
    return CHECK_DONE();
}
