#include "integrityd.h"
#include "check.h"

#include <stdlib.h>

int main(void)
{
    Str s;
    str_init(&s);
    CHECK(s.len == 0);
    str_append(&s, "abc");
    str_appendc(&s, '/');
    str_appendf(&s, "%d-%s", 42, "x");
    CHECK_STREQ(s.p, "abc/42-x");
    CHECK(s.len == 8);
    for (int i = 0; i < 10000; i++) str_appendc(&s, 'z');
    CHECK(s.len == 10008 && s.p[s.len] == '\0');
    str_clear(&s);
    CHECK(s.len == 0 && s.p[0] == '\0');
    str_appendn(&s, "a\0b", 3);
    CHECK(s.len == 3 && s.p[2] == 'b');
    char *stolen = str_steal(&s);
    CHECK(stolen[0] == 'a' && s.p == NULL);
    free(stolen);
    str_free(&s);

    Vec v;
    vec_init(&v);
    for (long i = 0; i < 1000; i++) vec_push(&v, (void *)i);
    CHECK(v.n == 1000 && (long)v.items[999] == 999);
    vec_free(&v);
    CHECK(v.n == 0 && v.items == NULL);

    char *j = path_join("/a/b", "c");
    CHECK_STREQ(j, "/a/b/c");
    free(j);
    j = path_join("/", "etc");
    CHECK_STREQ(j, "/etc");
    free(j);

    CHECK(path_under("/a/b", "/a"));
    CHECK(path_under("/a", "/a"));
    CHECK(!path_under("/ab", "/a"));
    CHECK(path_under("/x/y", "/"));
    CHECK_STREQ(path_rel("/a/b/c", "/a"), "b/c");
    CHECK_STREQ(path_rel("/a", "/a"), "");
    CHECK_STREQ(path_rel("/etc/x", "/"), "etc/x");
    CHECK_STREQ(path_base("/a/b/c.txt"), "c.txt");

    char buf[32];
    fmt_utc_ms(buf, 86400000LL + 1234);
    CHECK_STREQ(buf, "1970-01-02T00:00:01.234Z");
    fmt_utc(buf, 0);
    CHECK_STREQ(buf, "1970-01-01T00:00:00Z");
    CHECK(now_ms() > 0 && wall_ms() > 1600000000000LL);

    return CHECK_DONE();
}
