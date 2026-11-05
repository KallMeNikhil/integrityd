#include "integrityd.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static int selfpipe[2] = { -1, -1 };

static const int handled[] = { SIGTERM, SIGINT, SIGHUP, SIGUSR1, SIGUSR2, SIGALRM, SIGCHLD };

static void on_signal(int sig)
{
    int e = errno;
    unsigned char b = (unsigned char)sig;
    ssize_t r = write(selfpipe[1], &b, 1);
    (void)r;
    errno = e;
}

int signals_init(void)
{
    if (pipe2(selfpipe, O_NONBLOCK | O_CLOEXEC) < 0) return -1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    for (size_t i = 0; i < sizeof handled / sizeof *handled; i++) {
        sa.sa_flags = handled[i] == SIGCHLD ? SA_NOCLDSTOP : 0;
        if (sigaction(handled[i], &sa, NULL) < 0) return -1;
    }
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    return sigaction(SIGPIPE, &sa, NULL);
}

int signals_fd(void) { return selfpipe[0]; }

int signals_next(void)
{
    unsigned char b;
    for (;;) {
        ssize_t r = read(selfpipe[0], &b, 1);
        if (r == 1) return b;
        if (r < 0 && errno == EINTR) continue;
        return 0;
    }
}

void signals_child_default(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof handled / sizeof *handled; i++) sigaction(handled[i], &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    if (selfpipe[0] >= 0) close(selfpipe[0]);
    if (selfpipe[1] >= 0) close(selfpipe[1]);
    selfpipe[0] = selfpipe[1] = -1;
}
