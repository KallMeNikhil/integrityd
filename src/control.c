#include "integrityd.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int control_open(const char *fifo, int *rfd, int *wfd, Str *err)
{
    unlink(fifo);
    if (mkfifo(fifo, 0600) < 0 || chmod(fifo, 0600) < 0) {
        str_appendf(err, "mkfifo %s: %s", fifo, strerror(errno));
        return -1;
    }
    *rfd = open(fifo, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    *wfd = *rfd >= 0 ? open(fifo, O_WRONLY | O_CLOEXEC) : -1;
    if (*rfd < 0 || *wfd < 0) {
        str_appendf(err, "open %s: %s", fifo, strerror(errno));
        if (*rfd >= 0) close(*rfd);
        return -1;
    }
    return 0;
}

int control_read(int fd, ControlIn *in, void (*fn)(void *, const char *), void *ud)
{
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno == EAGAIN ? 0 : -1;
        }
        if (n == 0) return 0;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                if (!in->discarding && in->acc.len) {
                    if (in->acc.p[in->acc.len - 1] == '\r') in->acc.p[--in->acc.len] = '\0';
                    fn(ud, in->acc.p);
                }
                str_clear(&in->acc);
                in->discarding = 0;
            } else if (!in->discarding) {
                str_appendc(&in->acc, c);
                if (in->acc.len > CONTROL_MAX_LINE) {
                    in->discarding = 1;
                    str_clear(&in->acc);
                }
            }
        }
    }
}

void request_free(Request *req)
{
    free(req->id);
    free(req->reply);
    free(req->cmd);
    vec_free_all(&req->args, free);
    memset(req, 0, sizeof *req);
}

int control_parse(const char *line, Request *req, Str *err)
{
    memset(req, 0, sizeof *req);
    vec_init(&req->args);
    int field = 0, rc = 0;
    const char *p = line;
    while (*p) {
        const char *sp = strchr(p, ' ');
        size_t len = sp ? (size_t)(sp - p) : strlen(p);
        char *tok = xstrndup(p, len);
        p += len + (sp ? 1 : 0);
        if (len == 0 || (sp && !*p)) {
            if (!rc) str_append(err, "empty field");
            rc = -1;
        }
        Str val;
        str_init(&val);
        if (field != 0 && field != 2 && unescape(tok, &val) < 0) {
            if (!rc) str_append(err, "bad escape");
            rc = -1;
        }
        if (field == 0) req->id = tok;
        else if (field == 1) {
            req->reply = strcmp(tok, "-") == 0 ? NULL : xstrdup(val.p);
            free(tok);
        } else if (field == 2) {
            for (char *c = tok; *c; c++) *c = (char)toupper((unsigned char)*c);
            req->cmd = tok;
        } else {
            vec_push(&req->args, xstrdup(val.p));
            free(tok);
        }
        str_free(&val);
        field++;
    }
    if (field < 3) {
        if (!rc) str_append(err, "expected: <req-id> <reply-fifo|-> <COMMAND> [args]");
        rc = -1;
    }
    return rc;
}

void reply_init(Reply *r)
{
    str_init(&r->buf);
    r->is_err = 0;
}

void reply_ok(Reply *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *text = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(text, (size_t)n + 1, fmt, ap);
    va_end(ap);
    str_append(&r->buf, "OK ");
    for (char *c = text; *c; c++) str_appendc(&r->buf, *c == '\n' ? ' ' : *c);
    str_appendc(&r->buf, '\n');
    free(text);
}

void reply_err(Reply *r, int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *text = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(text, (size_t)n + 1, fmt, ap);
    va_end(ap);
    str_appendf(&r->buf, "ERR %d ", code);
    for (char *c = text; *c; c++) str_appendc(&r->buf, *c == '\n' ? ' ' : *c);
    str_appendc(&r->buf, '\n');
    r->is_err = 1;
    free(text);
}

void reply_free(Reply *r) { str_free(&r->buf); }

int control_send(const char *reply_dir, const char *reply_path, Reply *r)
{
    if (!reply_path) return 0;
    if (!path_under(reply_path, reply_dir) || strcmp(reply_path, reply_dir) == 0) return -1;
    int fd = open(reply_path, O_WRONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISFIFO(st.st_mode)) {
        close(fd);
        return -1;
    }
    str_append(&r->buf, ".\n");
    const char *p = r->buf.p;
    size_t left = r->buf.len;
    int64_t deadline = now_ms() + 1000;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w > 0) {
            p += w;
            left -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno != EAGAIN) break;
        int64_t remain = deadline - now_ms();
        if (remain <= 0) break;
        struct pollfd pfd = { fd, POLLOUT, 0 };
        if (poll(&pfd, 1, (int)remain) <= 0) break;
    }
    close(fd);
    return left ? -1 : 0;
}
