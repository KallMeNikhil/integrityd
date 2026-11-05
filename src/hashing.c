#include "integrityd.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define HASH_CHUNK (64 * 1024)
#define HASH_TRIES 3

static int same_stat(const struct stat *a, const struct stat *b)
{
    return a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

int hash_file(const char *path, int64_t max_bytes, char hex[65], struct stat *st, int *err)
{
    static unsigned char buf[HASH_CHUNK];
    struct stat before, after;
    int dummy;
    if (!err) err = &dummy;
    *err = 0;
    hex[0] = '\0';
    for (int attempt = 0; attempt < HASH_TRIES; attempt++) {
        int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
        if (fd < 0) {
            *err = errno;
            if (errno == ENOENT || errno == ENOTDIR) return HASH_GONE;
            if (errno == EACCES || errno == EPERM) return HASH_UNREADABLE;
            if (errno == ELOOP || errno == ENXIO) return HASH_NOTREG;
            return HASH_IOERR;
        }
        if (fstat(fd, &before) < 0) {
            *err = errno;
            close(fd);
            return HASH_IOERR;
        }
        if (!S_ISREG(before.st_mode)) {
            close(fd);
            return HASH_NOTREG;
        }
        if (st) *st = before;
        if (max_bytes > 0 && before.st_size > max_bytes) {
            close(fd);
            return HASH_TOOBIG;
        }
        Sha256 ctx;
        sha256_init(&ctx);
        for (;;) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n < 0) {
                if (errno == EINTR) continue;
                *err = errno;
                close(fd);
                return errno == EACCES || errno == EPERM ? HASH_UNREADABLE : HASH_IOERR;
            }
            if (n == 0) break;
            sha256_update(&ctx, buf, (size_t)n);
        }
        int rc = fstat(fd, &after);
        if (rc < 0) *err = errno;
        close(fd);
        if (rc < 0) return HASH_IOERR;
        if (same_stat(&before, &after)) {
            unsigned char d[32];
            sha256_final(&ctx, d);
            hex_encode(d, 32, hex);
            if (st) *st = after;
            return HASH_OK;
        }
    }
    return HASH_UNSTABLE;
}

const char *hash_status_name(int status)
{
    static const char *const names[] = { "ok", "unreadable", "unstable", "not a regular file",
                                         "vanished", "too big", "I/O error" };
    return status >= 0 && status <= HASH_IOERR ? names[status] : "?";
}
