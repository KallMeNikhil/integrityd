#include "integrityd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int hook_validate(const char *path, Str *why)
{
    struct stat st;
    if (path[0] != '/') {
        str_append(why, "not an absolute path");
        return -1;
    }
    if (stat(path, &st) < 0) {
        str_appendf(why, "%s", strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode)) str_append(why, "not a regular file");
    else if (access(path, X_OK) < 0) str_append(why, "not executable");
    else if (st.st_uid != geteuid() && st.st_uid != 0) str_append(why, "not owned by the daemon's user or root");
    else if (st.st_mode & 022) str_append(why, "group- or world-writable");
    else return 0;
    return -1;
}

void hook_set(Hook *h, const char *path, int min_sev, int rate)
{
    free(h->path);
    h->path = path ? xstrdup(path) : NULL;
    h->min_sev = min_sev;
    h->rate = rate;
}

int hook_fire(Hook *h, const char *event, const char *path, int sev, uint64_t seq, const char *detail)
{
    if (!h->path || sev < h->min_sev) return 0;
    int64_t now = now_ms();
    if (now - h->win_start >= 1000) {
        h->win_start = now;
        h->count = 0;
    }
    if (h->count >= h->rate) {
        h->dropped++;
        return 1;
    }
    h->count++;
    char seqs[32];
    snprintf(seqs, sizeof seqs, "%llu", (unsigned long long)seq);
    Str env;
    str_init(&env);
    str_append(&env, "INTEGRITYD_DETAIL=");
    str_append(&env, detail && *detail ? detail : "-");
    char *argv[] = { h->path, (char *)event, (char *)(path ? path : "-"), (char *)sev_name(sev), seqs, NULL };
    char *envp[] = { env.p, "PATH=/usr/bin:/bin", NULL };
    pid_t pid = fork();
    if (pid == 0) {
        signals_child_default();
        setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) close(fd);
        }
        execve(h->path, argv, envp);
        _exit(127);
    }
    str_free(&env);
    return 0;
}

int hook_take_dropped(Hook *h, int64_t now)
{
    if (!h->dropped || now - h->win_start < 1000) return 0;
    int n = h->dropped;
    h->dropped = 0;
    return n;
}
