#include "integrityd.h"
#include "check.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

static Vec got;
static void collect(void *ud, const char *line)
{
    (void)ud;
    vec_push(&got, xstrdup(line));
}

int main(void)
{
    Request q;
    Str err;
    str_init(&err);

    CHECK(control_parse("42 /run/reply/1.2 status", &q, &err) == 0);
    CHECK_STREQ(q.id, "42");
    CHECK_STREQ(q.reply, "/run/reply/1.2");
    CHECK_STREQ(q.cmd, "STATUS");
    CHECK(q.args.n == 0);
    request_free(&q);

    CHECK(control_parse("1 - ADD /a%20b/new%0Aline/p%25c", &q, &err) == 0);
    CHECK(q.reply == NULL);
    CHECK(q.args.n == 1);
    CHECK_STREQ(q.args.items[0], "/a b/new\nline/p%c");
    request_free(&q);

    CHECK(control_parse("1 /my%20run/reply/x TAIL 5", &q, &err) == 0);
    CHECK_STREQ(q.reply, "/my run/reply/x");
    CHECK_STREQ(q.args.items[0], "5");
    request_free(&q);

    str_clear(&err);
    CHECK(control_parse("1 /r/x", &q, &err) < 0 && strstr(err.p, "expected") != NULL);
    request_free(&q);
    CHECK(control_parse("1  /r/x PING", &q, &err) < 0);
    request_free(&q);
    CHECK(control_parse("1 /r/x PING ", &q, &err) < 0);
    request_free(&q);
    CHECK(control_parse("1 /r/x ADD /bad%zz", &q, &err) < 0);
    CHECK_STREQ(q.reply, "/r/x");
    request_free(&q);

    int fds[2];
    CHECK(pipe2(fds, O_NONBLOCK) == 0);
    ControlIn in;
    str_init(&in.acc);
    in.discarding = 0;
    vec_init(&got);
    const char *msg = "1 - PING\r\n2 - VERSION\n3 - par";
    CHECK(write(fds[1], msg, strlen(msg)) == (ssize_t)strlen(msg));
    control_read(fds[0], &in, collect, NULL);
    CHECK(got.n == 2);
    CHECK_STREQ(got.items[0], "1 - PING");
    CHECK_STREQ(got.items[1], "2 - VERSION");
    CHECK(write(fds[1], "tial\n", 5) == 5);
    control_read(fds[0], &in, collect, NULL);
    CHECK(got.n == 3 && strcmp(got.items[2], "3 - partial") == 0);
    char *big = xmalloc(6000);
    memset(big, 'x', 5999);
    big[5999] = '\n';
    CHECK(write(fds[1], big, 6000) == 6000);
    CHECK(write(fds[1], "4 - PING\n", 9) == 9);
    control_read(fds[0], &in, collect, NULL);
    CHECK(got.n == 4 && strcmp(got.items[3], "4 - PING") == 0);
    free(big);
    close(fds[0]);
    close(fds[1]);
    vec_free_all(&got, free);
    str_free(&in.acc);

    Reply r;
    reply_init(&r);
    reply_ok(&r, "pong");
    reply_ok(&r, "a\nb");
    reply_err(&r, 422, "bad %s", "arg");
    CHECK_STREQ(r.buf.p, "OK pong\nOK a b\nERR 422 bad arg\n");
    CHECK(r.is_err);
    CHECK(control_send("/run/reply", "/etc/passwd", &r) < 0);
    CHECK(control_send("/run/reply", "/run/reply", &r) < 0);
    CHECK(control_send("/run/reply", NULL, &r) == 0);
    reply_free(&r);

    str_free(&err);
    return CHECK_DONE();
}
