#include "integrityd.h"
#include "check.h"

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    char dir[] = "/tmp/itest_hash.XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char *f = path_join(dir, "f"), *fifo = path_join(dir, "fifo"), *ln = path_join(dir, "ln");
    char *big = path_join(dir, "big"), *gone = path_join(dir, "gone");
    char hex[65];
    struct stat st;
    int err;

    FILE *fp = fopen(f, "w");
    fputs("abc", fp);
    fclose(fp);
    CHECK(hash_file(f, 0, hex, &st, &err) == HASH_OK);
    CHECK_STREQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(st.st_size == 3);

    CHECK(hash_file(gone, 0, hex, &st, &err) == HASH_GONE && hex[0] == '\0');
    CHECK(mkfifo(fifo, 0600) == 0);
    alarm(5);
    CHECK(hash_file(fifo, 0, hex, &st, &err) == HASH_NOTREG);
    alarm(0);
    CHECK(symlink(f, ln) == 0);
    CHECK(hash_file(ln, 0, hex, &st, &err) == HASH_NOTREG);
    CHECK(hash_file(dir, 0, hex, &st, &err) == HASH_NOTREG);
    CHECK(hash_file(f, 2, hex, &st, &err) == HASH_TOOBIG && st.st_size == 3);
    if (geteuid() != 0) {
        chmod(f, 0);
        CHECK(hash_file(f, 0, hex, &st, &err) == HASH_UNREADABLE);
        chmod(f, 0644);
    }

    int fd = open(big, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char *chunk = calloc(1, 1 << 20);
    for (int i = 0; i < 16; i++) CHECK(write(fd, chunk, 1 << 20) == 1 << 20);
    close(fd);
    free(chunk);
    pid_t pid = fork();
    if (pid == 0) {
        int wfd = open(big, O_WRONLY | O_APPEND);
        for (;;) {
            if (write(wfd, "x", 1) < 0) _exit(1);
        }
    }
    usleep(50000);
    CHECK(hash_file(big, 0, hex, &st, &err) == HASH_UNSTABLE);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    CHECK(hash_file(big, 0, hex, &st, &err) == HASH_OK);

    CHECK(strcmp(hash_status_name(HASH_UNSTABLE), "unstable") == 0);

    unlink(f); unlink(fifo); unlink(ln); unlink(big);
    rmdir(dir);
    free(f); free(fifo); free(ln); free(big); free(gone);
    return CHECK_DONE();
}
