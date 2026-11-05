#include "integrityd.h"
#include "check.h"

#include <stdlib.h>

int main(void)
{
    HashTab *t = ht_new();
    char key[64];
    for (long i = 0; i < 20000; i++) {
        snprintf(key, sizeof key, "/path/%ld", i);
        CHECK(ht_put(t, key, (void *)(i + 1)) == NULL);
    }
    CHECK(ht_count(t) == 20000);
    int ok = 1;
    for (long i = 0; i < 20000; i++) {
        snprintf(key, sizeof key, "/path/%ld", i);
        if ((long)ht_get(t, key) != i + 1) ok = 0;
    }
    CHECK(ok);
    CHECK(ht_get(t, "/nope") == NULL);
    CHECK(!ht_has(t, "/nope"));

    CHECK((long)ht_put(t, "/path/7", (void *)99) == 8);
    CHECK((long)ht_get(t, "/path/7") == 99);
    CHECK(ht_count(t) == 20000);

    for (long i = 0; i < 20000; i += 2) {
        snprintf(key, sizeof key, "/path/%ld", i);
        ht_del(t, key);
    }
    CHECK(ht_count(t) == 10000);
    CHECK(!ht_has(t, "/path/0") && ht_has(t, "/path/1"));
    CHECK(ht_del(t, "/path/0") == NULL);

    HtIter it;
    const char *k;
    void *v;
    size_t n = 0;
    ht_iter_init(&it, t);
    while (ht_iter_next(&it, &k, &v)) n++;
    CHECK(n == 10000);

    Vec keys;
    vec_init(&keys);
    ht_keys(t, &keys);
    CHECK(keys.n == 10000);
    int sorted = 1;
    for (size_t i = 1; i < keys.n; i++)
        if (strcmp(keys.items[i - 1], keys.items[i]) >= 0) sorted = 0;
    CHECK(sorted);
    vec_free(&keys);

    HashTab *t2 = ht_new();
    ht_put(t2, "", xstrdup("empty"));
    ht_put(t2, "a", xstrdup("x"));
    CHECK_STREQ(ht_get(t2, ""), "empty");
    ht_free(t2, free);

    ht_free(t, NULL);
    return CHECK_DONE();
}
