#include "integrityd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SNAP_MAGIC "#INTEGRITYD-SNAPSHOT v1"

void snapshot_init(Snapshot *s)
{
    s->recs = ht_new();
    vec_init(&s->roots);
}

void snapshot_free(Snapshot *s)
{
    ht_free(s->recs, record_free);
    s->recs = NULL;
    vec_free_all(&s->roots, free);
}

void snapshot_put(Snapshot *s, Record *r)
{
    record_free(ht_put(s->recs, r->path, r));
}

void snapshot_del(Snapshot *s, const char *path)
{
    record_free(ht_del(s->recs, path));
}

void snapshot_sorted(const Snapshot *s, Vec *out)
{
    HtIter it;
    void *val;
    ht_iter_init(&it, s->recs);
    while (ht_iter_next(&it, NULL, &val)) vec_push(out, val);
    if (out->n > 1) qsort(out->items, out->n, sizeof *out->items, record_cmp);
}

const char *snapshot_root_of(const Snapshot *s, const char *path)
{
    const char *best = NULL;
    for (size_t i = 0; i < s->roots.n; i++) {
        const char *r = s->roots.items[i];
        if (path_under(path, r) && (!best || strlen(r) > strlen(best))) best = r;
    }
    return best;
}

int snapshot_add_root(Snapshot *s, const char *root)
{
    for (size_t i = 0; i < s->roots.n; i++)
        if (path_under(root, s->roots.items[i])) return 1;
    for (size_t i = 0; i < s->roots.n;) {
        if (path_under(s->roots.items[i], root)) snapshot_remove_root(s, s->roots.items[i]);
        else i++;
    }
    vec_push(&s->roots, xstrdup(root));
    qsort(s->roots.items, s->roots.n, sizeof *s->roots.items, path_cmp);
    return 0;
}

int snapshot_remove_root(Snapshot *s, const char *root)
{
    for (size_t i = 0; i < s->roots.n; i++) {
        if (strcmp(s->roots.items[i], root) != 0) continue;
        free(s->roots.items[i]);
        memmove(&s->roots.items[i], &s->roots.items[i + 1], (s->roots.n - i - 1) * sizeof *s->roots.items);
        s->roots.n--;
        return 0;
    }
    return -1;
}

void snapshot_drop_under(Snapshot *s, const char *root, size_t *count)
{
    Vec doomed;
    vec_init(&doomed);
    HtIter it;
    const char *k;
    ht_iter_init(&it, s->recs);
    while (ht_iter_next(&it, &k, NULL))
        if (path_under(k, root)) vec_push(&doomed, xstrdup(k));
    for (size_t i = 0; i < doomed.n; i++) snapshot_del(s, doomed.items[i]);
    if (count) *count = doomed.n;
    vec_free_all(&doomed, free);
}

static int put_line(FILE *f, Sha256 *sum, const Str *line)
{
    sha256_update(sum, line->p, line->len);
    return fputs(line->p, f) == EOF ? -1 : 0;
}

static void fsync_dir_of(const char *path)
{
    char *dir = xstrdup(path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        if (slash == dir) slash[1] = '\0';
        else *slash = '\0';
        int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
    }
    free(dir);
}

int snapshot_save(const Snapshot *s, const char *path, Str *err)
{
    Str tmp, line;
    str_init(&tmp);
    str_init(&line);
    str_appendf(&tmp, "%s.tmp", path);
    int fd = open(tmp.p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) {
        str_appendf(err, "%s: %s", tmp.p, strerror(errno));
        if (fd >= 0) close(fd);
        str_free(&tmp);
        return -1;
    }
    Sha256 sum;
    sha256_init(&sum);
    int bad = 0;
    char ts[32];
    fmt_utc(ts, time(NULL));
    str_append(&line, SNAP_MAGIC "\n");
    bad |= put_line(f, &sum, &line);
    for (size_t i = 0; i < s->roots.n; i++) {
        str_clear(&line);
        str_append(&line, "#ROOT ");
        escape_append(&line, s->roots.items[i], ESC_PATH);
        str_appendc(&line, '\n');
        bad |= put_line(f, &sum, &line);
    }
    str_clear(&line);
    str_appendf(&line, "#CREATED %s\n", ts);
    bad |= put_line(f, &sum, &line);
    Vec recs;
    vec_init(&recs);
    snapshot_sorted(s, &recs);
    for (size_t i = 0; i < recs.n && !bad; i++) {
        str_clear(&line);
        record_format(&line, recs.items[i]);
        str_appendc(&line, '\n');
        bad |= put_line(f, &sum, &line);
    }
    unsigned char d[32];
    char hex[65];
    sha256_final(&sum, d);
    hex_encode(d, 32, hex);
    if (fprintf(f, "#END %zu %s\n", recs.n, hex) < 0) bad = 1;
    vec_free(&recs);
    str_free(&line);
    if (fflush(f) == EOF || fsync(fileno(f)) < 0) bad = 1;
    int saved = errno;
    if (fclose(f) == EOF) bad = 1;
    if (!bad && rename(tmp.p, path) < 0) {
        saved = errno;
        bad = 1;
    }
    if (bad) {
        str_appendf(err, "writing %s: %s", path, strerror(saved ? saved : EIO));
        unlink(tmp.p);
        str_free(&tmp);
        return -1;
    }
    fsync_dir_of(path);
    str_free(&tmp);
    return 0;
}

int snapshot_load(Snapshot *s, const char *path, Str *err)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return SNAP_MISSING;
        str_appendf(err, "%s: %s", path, strerror(errno));
        return SNAP_IOERR;
    }
    Sha256 sum;
    sha256_init(&sum);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int lineno = 0, rc = SNAP_CORRUPT, ended = 0;
    size_t count = 0;
    const char *why = "unexpected end of file";
    while ((n = getline(&line, &cap, f)) >= 0) {
        lineno++;
        if (ended) {
            why = "data after #END";
            goto out;
        }
        if (n == 0 || line[n - 1] != '\n') {
            why = "truncated line";
            goto out;
        }
        if (strncmp(line, "#END ", 5) == 0) {
            unsigned char d[32];
            char hex[65], want[65];
            size_t want_count;
            sha256_final(&sum, d);
            hex_encode(d, 32, hex);
            if (sscanf(line + 5, "%zu %64s", &want_count, want) != 2) {
                why = "malformed #END";
                goto out;
            }
            if (want_count != count) {
                why = "record count mismatch";
                goto out;
            }
            if (strcmp(want, hex) != 0) {
                why = "checksum mismatch";
                goto out;
            }
            ended = 1;
            continue;
        }
        sha256_update(&sum, line, (size_t)n);
        line[n - 1] = '\0';
        if (lineno == 1) {
            if (strcmp(line, SNAP_MAGIC) != 0) {
                why = "bad magic line";
                goto out;
            }
        } else if (strncmp(line, "#ROOT ", 6) == 0) {
            Str root;
            str_init(&root);
            if (unescape(line + 6, &root) < 0 || root.p[0] != '/') {
                str_free(&root);
                why = "bad #ROOT line";
                goto out;
            }
            snapshot_add_root(s, root.p);
            str_free(&root);
        } else if (line[0] == '#') {
            continue;
        } else {
            Record *r;
            if (record_parse(line, &r) < 0) {
                why = "bad record line";
                goto out;
            }
            if (ht_has(s->recs, r->path)) {
                record_free(r);
                why = "duplicate path";
                goto out;
            }
            snapshot_put(s, r);
            count++;
        }
    }
    if (ended) rc = SNAP_OK;
out:
    if (rc != SNAP_OK) str_appendf(err, "%s: line %d: %s", path, lineno, why);
    free(line);
    fclose(f);
    return rc;
}
