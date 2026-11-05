#include "integrityd.h"
#include "check.h"

#include <stdlib.h>
#include <unistd.h>

static char *verify_out(const char *path, int *rc)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    *rc = verify_log(path, m);
    fclose(m);
    return buf;
}

static void rewrite_byte(const char *path, const char *needle, char c)
{
    Str s;
    str_init(&s);
    read_file(path, &s);
    char *p = strstr(s.p, needle);
    if (p) *p = c;
    FILE *f = fopen(path, "w");
    fputs(s.p, f);
    fclose(f);
    str_free(&s);
}

int main(void)
{
    char dir[] = "/tmp/itest_log.XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char *path = path_join(dir, "change.log");
    Logger l;
    Str err;
    str_init(&err);
    int rc;
    char *out;

    CHECK(logger_open(&l, path, 0, 0, 5, &err) == 0);
    CHECK(l.seq == 0);
    for (int i = 0; i < 5; i++) CHECK(logger_write(&l, SEV_WARN, "CREATED", "/w/f\tx", "size=1;sha=-") == 0);
    logger_write(&l, SEV_INFO, "STATUS", NULL, NULL);
    logger_close(&l);

    out = verify_out(path, &rc);
    CHECK(rc == 0 && strncmp(out, "OK 6 head=", 10) == 0);
    free(out);

    CHECK(logger_open(&l, path, 1, 0, 5, &err) == 0);
    CHECK(l.seq == 6);
    logger_write(&l, SEV_ALERT, "MODIFIED", "/w/f", "stealth=1");
    logger_close(&l);
    out = verify_out(path, &rc);
    CHECK(rc == 0 && strncmp(out, "OK 7 ", 5) == 0);
    free(out);

    Vec lines;
    vec_init(&lines);
    CHECK(log_tail(path, 3, &lines) == 0 && lines.n == 3);
    CHECK(strncmp(lines.items[0], "5\t", 2) == 0);
    CHECK(strstr(lines.items[0], "\t/w/f%09x\t") != NULL);
    CHECK(strstr(lines.items[1], "\tINFO\tSTATUS\t-\t-\t") != NULL);
    vec_free_all(&lines, free);

    Str s;
    str_init(&s);
    read_file(path, &s);
    char *rec3 = strstr(s.p, "\n3\t");
    char *f3 = strstr(rec3, "CREATED");
    f3[0] = 'X';
    FILE *f = fopen(path, "w");
    fputs(s.p, f);
    fclose(f);
    out = verify_out(path, &rc);
    CHECK(rc == 4 && strncmp(out, "BROKEN 3 ", 9) == 0);
    free(out);
    f3[0] = 'C';
    f = fopen(path, "w");
    fputs(s.p, f);
    fclose(f);
    str_free(&s);

    str_init(&s);
    read_file(path, &s);
    char *r4 = strstr(s.p, "\n4\t") + 1, *r5 = strstr(s.p, "\n5\t") + 1;
    memmove(r4, r5, strlen(r5) + 1);
    f = fopen(path, "w");
    fputs(s.p, f);
    fclose(f);
    out = verify_out(path, &rc);
    CHECK(rc == 4 && strncmp(out, "BROKEN 4 sequence gap", 21) == 0);
    free(out);
    str_free(&s);
    unlink(path);

    CHECK(logger_open(&l, path, 0, 0, 5, &err) == 0);
    logger_write(&l, SEV_INFO, "DAEMON_START", NULL, NULL);
    logger_write(&l, SEV_INFO, "STATUS", NULL, NULL);
    logger_close(&l);
    f = fopen(path, "a");
    fputs("3\t2026-01-01T00:00:00.000Z\tALERT\tMODIF", f);
    fclose(f);
    out = verify_out(path, &rc);
    CHECK(rc == 4 && strncmp(out, "TRUNCATED 3 ", 12) == 0);
    free(out);
    CHECK(logger_open(&l, path, 0, 0, 5, &err) == 0);
    CHECK(l.partial > 0 && l.seq == 2);
    logger_write(&l, SEV_WARN, "ERROR", NULL, "what=log_partial_line");
    logger_close(&l);
    out = verify_out(path, &rc);
    CHECK(rc == 0 && strncmp(out, "OK 3 ", 5) == 0);
    free(out);
    unlink(path);

    CHECK(logger_open(&l, path, 0, 600, 3, &err) == 0);
    for (int i = 0; i < 40; i++) logger_write(&l, SEV_WARN, "CREATED", "/w/some/file/name", "size=1");
    uint64_t last = l.seq;
    logger_close(&l);
    char *p1 = path_join(dir, "change.log.1"), *p3 = path_join(dir, "change.log.3"),
         *p4 = path_join(dir, "change.log.4");
    CHECK(access(p1, F_OK) == 0 && access(p3, F_OK) == 0 && access(p4, F_OK) != 0);
    vec_init(&lines);
    log_tail(p1, 1, &lines);
    CHECK(lines.n == 1 && strstr(lines.items[0], "\tLOG_ROTATE\t") != NULL);
    vec_free_all(&lines, free);
    str_init(&s);
    read_file(path, &s);
    CHECK(strstr(s.p, "\tLOG_ROTATE\t-\tcont=1\t") != NULL);
    str_free(&s);
    out = verify_out(path, &rc);
    CHECK(rc == 0 && strstr(out, "anchor=seq") != NULL);
    free(out);
    CHECK(logger_open(&l, path, 0, 600, 3, &err) == 0 && l.seq == last);
    logger_close(&l);
    rewrite_byte(p1, "CREATED", 'Z');
    out = verify_out(path, &rc);
    CHECK(rc == 4 && strncmp(out, "BROKEN ", 7) == 0);
    free(out);

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    CHECK(system(cmd) == 0);
    out = verify_out(path, &rc);
    CHECK(rc == 2);
    free(out);
    free(p1); free(p3); free(p4);
    free(path);
    str_free(&err);
    return CHECK_DONE();
}
