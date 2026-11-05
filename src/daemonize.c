#include "integrityd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

pid_t pidfile_read(const char *path)
{
    Str s;
    str_init(&s);
    pid_t pid = 0;
    if (read_file(path, &s) == 0) {
        char *end;
        long v = strtol(s.p, &end, 10);
        if (end != s.p && v > 0) pid = (pid_t)v;
    }
    str_free(&s);
    return pid;
}

int pidfile_check(const char *path)
{
    pid_t pid = pidfile_read(path);
    if (pid > 0 && (kill(pid, 0) == 0 || errno == EPERM)) {
        Str comm, cpath;
        str_init(&comm);
        str_init(&cpath);
        str_appendf(&cpath, "/proc/%d/comm", (int)pid);
        int live = read_file(cpath.p, &comm) == 0 && strncmp(comm.p, "integrityd", 10) == 0 &&
                   (comm.p[10] == '\n' || comm.p[10] == '\0');
        str_free(&comm);
        str_free(&cpath);
        if (live) return 1;
    }
    if (access(path, F_OK) == 0) unlink(path);
    return 0;
}

int pidfile_write(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%d\n", (int)getpid());
    int rc = write_all(fd, buf, (size_t)n);
    close(fd);
    return rc;
}

static void close_inherited_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        int self = dirfd(d);
        Vec fds;
        vec_init(&fds);
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            int fd = atoi(de->d_name);
            if (fd >= 3 && fd != self) vec_push(&fds, (void *)(intptr_t)fd);
        }
        closedir(d);
        for (size_t i = 0; i < fds.n; i++) close((int)(intptr_t)fds.items[i]);
        vec_free(&fds);
        return;
    }
    long max = sysconf(_SC_OPEN_MAX);
    for (long fd = 3; fd < (max > 0 ? max : 1024); fd++) close((int)fd);
}

int daemonize(const char *out_path)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);
    if (setsid() < 0) return -1;
    signal(SIGHUP, SIG_IGN);
    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);
    umask(077);
    if (chdir("/") < 0) return -1;
    close_inherited_fds();
    int fd = open("/dev/null", O_RDWR);
    if (fd < 0 || dup2(fd, 0) < 0) return -1;
    if (fd > 2) close(fd);
    fd = open(out_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0) return -1;
    if (fd > 2) close(fd);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    return 0;
}
