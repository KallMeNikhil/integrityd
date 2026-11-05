#include "integrityd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char ZERO_CHAIN[] = "0000000000000000000000000000000000000000000000000000000000000000";

static void chain_next(const char *prev, const char *text, size_t len, char out[65])
{
    Sha256 c;
    unsigned char d[32];
    sha256_init(&c);
    sha256_update(&c, prev, 64);
    sha256_update(&c, "\n", 1);
    sha256_update(&c, text, len);
    sha256_final(&c, d);
    hex_encode(d, 32, out);
}

static int parse_line(const char *line, uint64_t *seq, char chain[65], size_t *text_len)
{
    int tabs = 0;
    for (const char *p = line; *p; p++) tabs += *p == '\t';
    if (tabs != 6) return -1;
    const char *last = strrchr(line, '\t');
    if (strlen(last + 1) != 64 || strspn(last + 1, "0123456789abcdef") != 64) return -1;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(line, &end, 10);
    if (errno || end == line || *end != '\t' || line[0] == '-') return -1;
    *seq = v;
    memcpy(chain, last + 1, 65);
    *text_len = (size_t)(last - line);
    return 0;
}

static int scan_tail(const char *path, uint64_t *seq, char chain[65], int64_t *good_end, int64_t *partial)
{
    FILE *f = fopen(path, "r");
    *good_end = 0;
    *partial = 0;
    if (!f) return errno == ENOENT ? 0 : -1;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int64_t off = 0;
    int found = 0;
    while ((n = getline(&line, &cap, f)) >= 0) {
        if (line[n - 1] != '\n') {
            *partial = n;
            break;
        }
        off += n;
        *good_end = off;
        line[n - 1] = '\0';
        uint64_t s;
        char c[65];
        size_t tl;
        if (parse_line(line, &s, c, &tl) == 0) {
            *seq = s;
            memcpy(chain, c, 65);
            found = 1;
        }
    }
    free(line);
    fclose(f);
    return found;
}

static int open_append(Logger *l, Str *err)
{
    int fd = open(l->path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0 || !(l->fp = fdopen(fd, "a"))) {
        str_appendf(err, "%s: %s", l->path, strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    struct stat st;
    l->size = fstat(fd, &st) == 0 ? (int64_t)st.st_size : 0;
    return 0;
}

int logger_open(Logger *l, const char *path, int do_fsync, int64_t max_bytes, int keep, Str *err)
{
    memset(l, 0, sizeof *l);
    l->path = xstrdup(path);
    l->do_fsync = do_fsync;
    l->max_bytes = max_bytes;
    l->keep = keep;
    memcpy(l->chain, ZERO_CHAIN, 65);
    int64_t good_end, partial;
    int found = scan_tail(path, &l->seq, l->chain, &good_end, &partial);
    if (found < 0) {
        str_appendf(err, "%s: %s", path, strerror(errno));
        free(l->path);
        l->path = NULL;
        return -1;
    }
    if (partial > 0) {
        if (truncate(path, good_end) < 0) {
            str_appendf(err, "%s: truncate: %s", path, strerror(errno));
            free(l->path);
            l->path = NULL;
            return -1;
        }
        l->partial = partial;
    }
    if (!found) {
        Str prev;
        str_init(&prev);
        str_appendf(&prev, "%s.1", path);
        int64_t g, p;
        scan_tail(prev.p, &l->seq, l->chain, &g, &p);
        str_free(&prev);
    }
    if (open_append(l, err) < 0) {
        free(l->path);
        l->path = NULL;
        return -1;
    }
    return 0;
}

void logger_stream(Logger *l, FILE *fp)
{
    memset(l, 0, sizeof *l);
    l->fp = fp;
    memcpy(l->chain, ZERO_CHAIN, 65);
}

static int rotate(Logger *l);

int logger_write(Logger *l, int sev, const char *event, const char *path, const char *detail)
{
    char ts[32];
    char chain[65];
    Str line;
    str_init(&line);
    fmt_utc_ms(ts, wall_ms());
    str_appendf(&line, "%llu\t%s\t%s\t%s\t", (unsigned long long)(l->seq + 1), ts, sev_name(sev), event);
    if (path) escape_append(&line, path, ESC_PATH);
    else str_appendc(&line, '-');
    str_appendc(&line, '\t');
    str_append(&line, detail && *detail ? detail : "-");
    chain_next(l->chain, line.p, line.len, chain);
    str_appendf(&line, "\t%s\n", chain);
    int rc = 0;
    if (fputs(line.p, l->fp) == EOF || fflush(l->fp) == EOF || (l->do_fsync && fsync(fileno(l->fp)) < 0)) {
        fprintf(stderr, "integrityd: ERROR what=io;file=change.log;errno=%d (%s)\n", errno, strerror(errno));
        clearerr(l->fp);
        rc = -1;
    } else {
        l->seq++;
        memcpy(l->chain, chain, 65);
        l->size += (int64_t)line.len;
    }
    str_free(&line);
    if (rc == 0 && l->path && l->max_bytes > 0 && l->size > l->max_bytes && !l->rotating) rotate(l);
    return rc;
}

static int rotate(Logger *l)
{
    l->rotating = 1;
    Str detail, from, to;
    str_init(&detail);
    str_init(&from);
    str_init(&to);
    str_appendf(&detail, "bytes=%lld;next=%s.1", (long long)l->size, path_base(l->path));
    logger_write(l, SEV_INFO, "LOG_ROTATE", NULL, detail.p);
    fclose(l->fp);
    l->fp = NULL;
    str_appendf(&from, "%s.%d", l->path, l->keep);
    unlink(from.p);
    for (int i = l->keep - 1; i >= 1; i--) {
        str_clear(&from);
        str_clear(&to);
        str_appendf(&from, "%s.%d", l->path, i);
        str_appendf(&to, "%s.%d", l->path, i + 1);
        if (rename(from.p, to.p) < 0 && errno != ENOENT)
            fprintf(stderr, "integrityd: rotate %s: %s\n", from.p, strerror(errno));
    }
    str_clear(&to);
    str_appendf(&to, "%s.1", l->path);
    int rc = rename(l->path, to.p);
    Str err;
    str_init(&err);
    if (open_append(l, &err) < 0) {
        fprintf(stderr, "integrityd: ERROR what=io;%s\n", err.p);
        rc = -1;
    } else {
        logger_write(l, SEV_INFO, "LOG_ROTATE", NULL, "cont=1");
    }
    str_free(&err);
    str_free(&detail);
    str_free(&from);
    str_free(&to);
    l->rotating = 0;
    return rc;
}

void logger_close(Logger *l)
{
    if (l->path && l->fp) fclose(l->fp);
    l->fp = NULL;
    free(l->path);
    l->path = NULL;
}

int verify_log(const char *path, FILE *out)
{
    Vec files;
    vec_init(&files);
    int maxn = 0;
    for (int i = 1; i < 1000; i++) {
        Str p;
        str_init(&p);
        str_appendf(&p, "%s.%d", path, i);
        int exists = access(p.p, F_OK) == 0;
        str_free(&p);
        if (!exists) break;
        maxn = i;
    }
    for (int i = maxn; i >= 1; i--) {
        Str p;
        str_init(&p);
        str_appendf(&p, "%s.%d", path, i);
        vec_push(&files, str_steal(&p));
    }
    if (access(path, F_OK) == 0) vec_push(&files, xstrdup(path));
    if (files.n == 0) {
        fprintf(out, "no log at %s\n", path);
        vec_free(&files);
        return 2;
    }
    char prev[65];
    uint64_t prev_seq = 0, records = 0, anchor = 0;
    int rc = 0;
    char *line = NULL;
    size_t cap = 0;
    for (size_t fi = 0; fi < files.n && rc == 0; fi++) {
        FILE *f = fopen(files.items[fi], "r");
        if (!f) {
            fprintf(out, "cannot open %s: %s\n", (char *)files.items[fi], strerror(errno));
            rc = 2;
            break;
        }
        ssize_t n;
        int lineno = 0;
        while ((n = getline(&line, &cap, f)) >= 0) {
            lineno++;
            if (line[n - 1] != '\n') {
                fprintf(out, "TRUNCATED %llu partial line at %s:%d\n", (unsigned long long)(prev_seq + 1),
                        (char *)files.items[fi], lineno);
                rc = 4;
                break;
            }
            line[n - 1] = '\0';
            uint64_t seq;
            char chain[65], want[65];
            size_t tl;
            if (parse_line(line, &seq, chain, &tl) < 0) {
                fprintf(out, "BROKEN %llu malformed line at %s:%d\n", (unsigned long long)(prev_seq + 1),
                        (char *)files.items[fi], lineno);
                rc = 4;
                break;
            }
            if (records == 0 && seq != 1) {
                anchor = seq;
                memcpy(prev, chain, 65);
            } else {
                if (records == 0) memcpy(prev, ZERO_CHAIN, 65);
                else if (seq != prev_seq + 1) {
                    fprintf(out, "BROKEN %llu sequence gap (found %llu) at %s:%d\n",
                            (unsigned long long)(prev_seq + 1), (unsigned long long)seq,
                            (char *)files.items[fi], lineno);
                    rc = 4;
                    break;
                }
                chain_next(prev, line, tl, want);
                if (strcmp(want, chain) != 0) {
                    fprintf(out, "BROKEN %llu chain mismatch at %s:%d\n", (unsigned long long)seq,
                            (char *)files.items[fi], lineno);
                    rc = 4;
                    break;
                }
                memcpy(prev, chain, 65);
            }
            prev_seq = seq;
            records++;
        }
        fclose(f);
    }
    free(line);
    if (rc == 0) {
        fprintf(out, "OK %llu head=%s", (unsigned long long)records, records ? prev : ZERO_CHAIN);
        if (anchor) fprintf(out, " anchor=seq %llu", (unsigned long long)anchor);
        fputc('\n', out);
    }
    vec_free_all(&files, free);
    return rc;
}

int log_tail(const char *path, int n, Vec *lines)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char **ring = xcalloc((size_t)n, sizeof *ring);
    size_t count = 0;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (len && line[len - 1] == '\n') line[len - 1] = '\0';
        free(ring[count % (size_t)n]);
        ring[count % (size_t)n] = xstrdup(line);
        count++;
    }
    free(line);
    fclose(f);
    size_t start = count > (size_t)n ? count - (size_t)n : 0;
    for (size_t i = start; i < count; i++) {
        vec_push(lines, ring[i % (size_t)n]);
        ring[i % (size_t)n] = NULL;
    }
    for (int i = 0; i < n; i++) free(ring[i]);
    free(ring);
    return 0;
}
