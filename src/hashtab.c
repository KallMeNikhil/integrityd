#include "integrityd.h"

#include <stdlib.h>
#include <string.h>

typedef struct Node {
    struct Node *next;
    uint64_t     hash;
    void        *val;
    char         key[];
} Node;

struct HashTab {
    Node **b;
    size_t nb, n;
};

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

HashTab *ht_new(void)
{
    HashTab *t = xmalloc(sizeof *t);
    t->nb = 64;
    t->n = 0;
    t->b = xcalloc(t->nb, sizeof *t->b);
    return t;
}

void ht_free(HashTab *t, void (*free_val)(void *))
{
    if (!t) return;
    for (size_t i = 0; i < t->nb; i++) {
        Node *n = t->b[i];
        while (n) {
            Node *next = n->next;
            if (free_val) free_val(n->val);
            free(n);
            n = next;
        }
    }
    free(t->b);
    free(t);
}

static Node *find(const HashTab *t, const char *key, uint64_t h)
{
    for (Node *n = t->b[h & (t->nb - 1)]; n; n = n->next)
        if (n->hash == h && strcmp(n->key, key) == 0) return n;
    return NULL;
}

void *ht_get(const HashTab *t, const char *key)
{
    Node *n = find(t, key, fnv1a(key));
    return n ? n->val : NULL;
}

int ht_has(const HashTab *t, const char *key)
{
    return find(t, key, fnv1a(key)) != NULL;
}

static void grow(HashTab *t)
{
    size_t nb = t->nb * 2;
    Node **b = xcalloc(nb, sizeof *b);
    for (size_t i = 0; i < t->nb; i++) {
        Node *n = t->b[i];
        while (n) {
            Node *next = n->next;
            n->next = b[n->hash & (nb - 1)];
            b[n->hash & (nb - 1)] = n;
            n = next;
        }
    }
    free(t->b);
    t->b = b;
    t->nb = nb;
}

void *ht_put(HashTab *t, const char *key, void *val)
{
    uint64_t h = fnv1a(key);
    Node *n = find(t, key, h);
    if (n) {
        void *old = n->val;
        n->val = val;
        return old;
    }
    if ((t->n + 1) * 4 > t->nb * 3) grow(t);
    size_t len = strlen(key);
    n = xmalloc(sizeof *n + len + 1);
    memcpy(n->key, key, len + 1);
    n->hash = h;
    n->val = val;
    n->next = t->b[h & (t->nb - 1)];
    t->b[h & (t->nb - 1)] = n;
    t->n++;
    return NULL;
}

void *ht_del(HashTab *t, const char *key)
{
    uint64_t h = fnv1a(key);
    Node **pp = &t->b[h & (t->nb - 1)];
    for (; *pp; pp = &(*pp)->next) {
        Node *n = *pp;
        if (n->hash == h && strcmp(n->key, key) == 0) {
            void *val = n->val;
            *pp = n->next;
            free(n);
            t->n--;
            return val;
        }
    }
    return NULL;
}

size_t ht_count(const HashTab *t) { return t->n; }

void ht_iter_init(HtIter *it, const HashTab *t)
{
    it->t = t;
    it->bucket = 0;
    it->node = NULL;
}

int ht_iter_next(HtIter *it, const char **key, void **val)
{
    Node *n = it->node ? ((Node *)it->node)->next : NULL;
    while (!n && it->bucket < it->t->nb) n = it->t->b[it->bucket++];
    it->node = n;
    if (!n) return 0;
    if (key) *key = n->key;
    if (val) *val = n->val;
    return 1;
}

void ht_keys(const HashTab *t, Vec *out)
{
    HtIter it;
    const char *k;
    ht_iter_init(&it, t);
    while (ht_iter_next(&it, &k, NULL)) vec_push(out, (void *)k);
    if (out->n > 1) qsort(out->items, out->n, sizeof *out->items, path_cmp);
}
