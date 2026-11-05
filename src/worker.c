#include "integrityd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void worker_init(Worker *w)
{
    memset(w, 0, sizeof *w);
    w->pid = 0;
    w->fd = -1;
    str_init(&w->buf);
}

int worker_running(const Worker *w) { return w->pid > 0; }

static void child(int fd, const Snapshot *s, int64_t max_bytes)
{
    Str line;
    str_init(&line);
    size_t count = 0;
    HtIter it;
    void *val;
    ht_iter_init(&it, s->recs);
    while (ht_iter_next(&it, NULL, &val)) {
        const Record *r = val;
        if (r->type != 'f') continue;
        char hex[65];
        struct stat st;
        if (hash_file(r->path, max_bytes, hex, &st, NULL) != HASH_OK) continue;
        str_clear(&line);
        escape_append(&line, r->path, ESC_PATH);
        str_appendf(&line, "\t%s\t%lld\t%lld\t%lld\n", hex, (long long)st.st_size,
                    (long long)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec,
                    (long long)st.st_ctim.tv_sec * 1000000000 + st.st_ctim.tv_nsec);
        if (write_all(fd, line.p, line.len) < 0) _exit(1);
        count++;
    }
    str_clear(&line);
    str_appendf(&line, "#DONE %zu\n", count);
    if (write_all(fd, line.p, line.len) < 0) _exit(1);
    _exit(0);
}

int worker_start(Worker *w, const Snapshot *s, int64_t max_bytes, const int *close_fds, int nclose)
{
    if (worker_running(w)) return -1;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        signals_child_default();
        close(fds[0]);
        for (int i = 0; i < nclose; i++)
            if (close_fds[i] >= 0) close(close_fds[i]);
        child(fds[1], s, max_bytes);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    str_clear(&w->buf);
    w->pid = pid;
    w->fd = fds[0];
    w->done_seen = w->eof = w->reaped = w->status = 0;
    w->count = 0;
    w->start_ms = now_ms();
    return 0;
}

static void parse_line(Worker *w, char *line, worker_line_fn fn, void *ud)
{
    if (strncmp(line, "#DONE ", 6) == 0) {
        w->done_seen = 1;
        return;
    }
    char *f[5];
    int n = 0;
    for (char *p = line; n < 5; n++) {
        f[n] = p;
        char *tab = strchr(p, '\t');
        if (!tab) {
            n++;
            break;
        }
        *tab = '\0';
        p = tab + 1;
    }
    if (n != 5) return;
    Str path;
    str_init(&path);
    if (unescape(f[0], &path) == 0) {
        w->count++;
        fn(ud, path.p, f[1], strtoll(f[2], NULL, 10), strtoll(f[3], NULL, 10), strtoll(f[4], NULL, 10));
    }
    str_free(&path);
}

int worker_read(Worker *w, worker_line_fn fn, void *ud)
{
    char buf[16384];
    for (;;) {
        ssize_t n = read(w->fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) return 0;
            n = 0;
        }
        if (n == 0) {
            close(w->fd);
            w->fd = -1;
            w->eof = 1;
            str_clear(&w->buf);
            return 1;
        }
        str_appendn(&w->buf, buf, (size_t)n);
        char *start = w->buf.p, *nl;
        while ((nl = memchr(start, '\n', w->buf.len - (size_t)(start - w->buf.p))) != NULL) {
            *nl = '\0';
            parse_line(w, start, fn, ud);
            start = nl + 1;
        }
        size_t rest = w->buf.len - (size_t)(start - w->buf.p);
        memmove(w->buf.p, start, rest);
        w->buf.len = rest;
        w->buf.p[rest] = '\0';
    }
}
