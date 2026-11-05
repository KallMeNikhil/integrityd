#include "integrityd.h"
#include "check.h"

#include <stdlib.h>

int main(void)
{
    Str s, u;
    str_init(&s);
    str_init(&u);

    escape_append(&s, "a b\tc\nd%e;f=g\x7f\x01", ESC_PATH);
    CHECK_STREQ(s.p, "a b%09c%0Ad%25e;f=g%7F%01");
    str_clear(&s);
    escape_append(&s, "k;v=w x", ESC_DETAIL);
    CHECK_STREQ(s.p, "k%3Bv%3Dw x");
    str_clear(&s);
    escape_append(&s, "a b", ESC_ARG);
    CHECK_STREQ(s.p, "a%20b");
    str_clear(&s);
    escape_append(&s, "caf\xc3\xa9", ESC_PATH);
    CHECK_STREQ(s.p, "caf\xc3\xa9");

    CHECK(unescape("%zz", &u) < 0);
    str_clear(&u);
    CHECK(unescape("abc%2", &u) < 0);
    str_clear(&u);
    CHECK(unescape("%00", &u) < 0);
    str_clear(&u);
    CHECK(unescape("%41%62c", &u) == 0);
    CHECK_STREQ(u.p, "Abc");

    srand(12345);
    const char *sets[] = { ESC_PATH, ESC_DETAIL, ESC_ARG };
    int all_ok = 1, clean = 1;
    for (int iter = 0; iter < 3000; iter++) {
        char raw[80];
        int n = rand() % 79;
        for (int i = 0; i < n; i++) raw[i] = (char)(1 + rand() % 255);
        raw[n] = '\0';
        const char *set = sets[iter % 3];
        str_clear(&s);
        str_clear(&u);
        escape_append(&s, raw, set);
        for (size_t i = 0; i < s.len; i++) {
            unsigned char c = (unsigned char)s.p[i];
            if (c < 0x20 || c == 0x7f || c == '\t' || (set[0] && strchr(set, c))) clean = 0;
        }
        if (unescape(s.p, &u) != 0 || strcmp(u.p, raw) != 0) all_ok = 0;
    }
    CHECK(all_ok);
    CHECK(clean);

    Str d;
    str_init(&d);
    detail_add(&d, "from", "/a;b=c");
    detail_addf(&d, "size", "%d", 12);
    CHECK_STREQ(d.p, "from=/a%3Bb%3Dc;size=12");
    str_clear(&u);
    CHECK(detail_get(d.p, "from", &u) == 1);
    CHECK_STREQ(u.p, "/a;b=c");
    str_clear(&u);
    CHECK(detail_get(d.p, "size", &u) == 1 && strcmp(u.p, "12") == 0);
    CHECK(detail_get(d.p, "siz", &u) == 0);
    CHECK(detail_get(d.p, "nope", &u) == 0);

    str_free(&d);
    str_free(&s);
    str_free(&u);
    return CHECK_DONE();
}
