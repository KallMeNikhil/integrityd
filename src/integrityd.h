#ifndef INTEGRITYD_H
#define INTEGRITYD_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>

#define INTEGRITYD_VERSION "integrityd 1.0.0"
#define CONTROL_MAX_LINE 4096

typedef struct { char *p; size_t len, cap; } Str;
void  str_init(Str *s);
void  str_appendn(Str *s, const char *t, size_t n);
void  str_append(Str *s, const char *t);
void  str_appendc(Str *s, char c);
void  str_appendf(Str *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void  str_clear(Str *s);
void  str_free(Str *s);
char *str_steal(Str *s);

typedef struct { void **items; size_t n, cap; } Vec;
void vec_init(Vec *v);
void vec_push(Vec *v, void *item);
void vec_free(Vec *v);
void vec_free_all(Vec *v, void (*free_item)(void *));

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void  die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void  warnmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

int64_t now_ms(void);
int64_t wall_ms(void);
void    fmt_utc_ms(char buf[32], int64_t ms);
void    fmt_utc(char buf[32], time_t t);

char       *path_join(const char *dir, const char *name);
int         path_under(const char *path, const char *root);
const char *path_base(const char *path);
const char *path_rel(const char *path, const char *root);
int         path_cmp(const void *a, const void *b);
int         mkdir_p(const char *path, mode_t mode);
int         write_all(int fd, const void *buf, size_t n);
int         read_file(const char *path, Str *out);
char       *default_rundir(void);

typedef struct HashTab HashTab;
typedef struct { const HashTab *t; size_t bucket; void *node; } HtIter;
HashTab *ht_new(void);
void     ht_free(HashTab *t, void (*free_val)(void *));
void    *ht_get(const HashTab *t, const char *key);
int      ht_has(const HashTab *t, const char *key);
void    *ht_put(HashTab *t, const char *key, void *val);
void    *ht_del(HashTab *t, const char *key);
size_t   ht_count(const HashTab *t);
void     ht_iter_init(HtIter *it, const HashTab *t);
int      ht_iter_next(HtIter *it, const char **key, void **val);
void     ht_keys(const HashTab *t, Vec *out);

enum { MODE_SCAN, MODE_WATCH, MODE_HYBRID };
enum { STARTUP_DEEP, STARTUP_META, STARTUP_NONE };
enum { SEV_INFO, SEV_WARN, SEV_ALERT };

typedef struct Config {
    Vec     watch;
    Vec     exclude;
    int     mode;
    int     scan_interval;
    int     deep_every;
    int     startup_check;
    int     batch_ms, batch_max_ms;
    int     snapshot_flush_secs;
    int     log_fsync;
    int64_t log_max_bytes;
    int     log_keep;
    int64_t hash_max_bytes;
    int     one_filesystem;
    int     max_depth;
    int     deep_verify_in_worker;
    int64_t worker_min_records;
    char   *alert_hook;
    int     hook_min_severity;
    int     hook_rate_per_sec;
} Config;

void        config_init(Config *c);
void        config_free(Config *c);
int         config_set(Config *c, const char *key, const char *val, Str *err);
int         config_load(Config *c, const char *path, Str *err);
int         config_excluded(const Config *c, const char *relpath, int is_dir);
int         parse_mode(const char *s);
const char *mode_name(int mode);
const char *sev_name(int sev);
int         sev_from_name(const char *s);

typedef struct { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t n; } Sha256;
void sha256_init(Sha256 *c);
void sha256_update(Sha256 *c, const void *data, size_t n);
void sha256_final(Sha256 *c, unsigned char out[32]);
void sha256_hex(const void *data, size_t n, char hex[65]);
void hex_encode(const unsigned char *in, size_t n, char *out);

enum { HASH_OK, HASH_UNREADABLE, HASH_UNSTABLE, HASH_NOTREG, HASH_GONE, HASH_TOOBIG, HASH_IOERR };
int         hash_file(const char *path, int64_t max_bytes, char hex[65], struct stat *st, int *err);
const char *hash_status_name(int status);

typedef struct Record {
    char     type;
    char    *path;
    char    *target;
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid;
    int64_t  size, mtime_ns, ctime_ns;
    char     sha[65];
} Record;

Record *record_new(const char *path);
Record *record_dup(const Record *r);
void    record_free(void *r);
void    record_set_stat(Record *r, const struct stat *st);
int     record_from_path(const char *path, Record **out, int *err);
int     record_cmp(const void *a, const void *b);
int     record_tuple_eq(const Record *a, const Record *b);

typedef struct ScanCtx {
    const Config *cfg;
    const char   *skip;
} ScanCtx;

typedef struct ScanResult {
    Vec recs;
    Vec unreadable;
    Vec errs;
    int root_missing;
} ScanResult;

void scan_result_init(ScanResult *r);
void scan_result_free(ScanResult *r);
int  scan_tree(const ScanCtx *ctx, const char *root, const char *start, ScanResult *out);
int  scan_excluded(const ScanCtx *ctx, const char *root, const char *path, int is_dir);

#define ESC_PATH   ""
#define ESC_DETAIL ";="
#define ESC_ARG    " "
void escape_append(Str *out, const char *s, const char *extra);
int  unescape(const char *s, Str *out);
void detail_add(Str *d, const char *key, const char *val);
void detail_addf(Str *d, const char *key, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int  detail_get(const char *detail, const char *key, Str *out);

typedef struct Snapshot {
    HashTab *recs;
    Vec      roots;
} Snapshot;

enum { SNAP_OK, SNAP_MISSING, SNAP_CORRUPT, SNAP_IOERR };
void snapshot_init(Snapshot *s);
void snapshot_free(Snapshot *s);
void snapshot_put(Snapshot *s, Record *r);
void snapshot_del(Snapshot *s, const char *path);
void snapshot_sorted(const Snapshot *s, Vec *out);
int  snapshot_load(Snapshot *s, const char *path, Str *err);
int  snapshot_save(const Snapshot *s, const char *path, Str *err);
void record_format(Str *out, const Record *r);
int  record_parse(const char *line, Record **out);
const char *snapshot_root_of(const Snapshot *s, const char *path);
int  snapshot_add_root(Snapshot *s, const char *root);
int  snapshot_remove_root(Snapshot *s, const char *root);
void snapshot_drop_under(Snapshot *s, const char *root, size_t *count);

typedef struct Logger {
    char    *path;
    FILE    *fp;
    uint64_t seq;
    char     chain[65];
    int      do_fsync;
    int64_t  max_bytes;
    int      keep;
    int64_t  size;
    int      rotating;
    int64_t  partial;
} Logger;

int  logger_open(Logger *l, const char *path, int do_fsync, int64_t max_bytes, int keep, Str *err);
void logger_stream(Logger *l, FILE *fp);
int  logger_write(Logger *l, int sev, const char *event, const char *path, const char *detail);
void logger_close(Logger *l);
int  verify_log(const char *path, FILE *out);
int  log_tail(const char *path, int n, Vec *lines);

enum { EV_CREATED, EV_MODIFIED, EV_DELETED, EV_PERMS, EV_TYPE_CHANGED, EV_MOVED,
       EV_REPLACED, EV_UNREADABLE, EV_UNSTABLE, EV_COUNT };

typedef struct Change {
    int           ev;
    char         *path;
    const Record *old;
    const Record *nw;
    int           stealth, basis_meta, err;
} Change;

const char *ev_name(int ev);
int         ev_severity(int ev);
void        classify(const Record *old, const Record *nw, Vec *out);
void        pair_moves(Vec *changes);
void        change_free(void *c);

typedef void (*emit_fn)(void *ud, int sev, const char *event, const char *path, const char *detail);

typedef struct Engine {
    Snapshot      *snap;
    const Config  *cfg;
    ScanCtx        ctx;
    emit_fn        emit;
    void          *ud;
    HashTab       *unreadable_seen;
    const char    *extra;
    int            silent;
    Vec            changes;
    Vec            newrecs;
    Vec            updates;
    HashTab       *batch;
    size_t         n_files, n_changes, n_added, n_removed, n_changed;
    int            dirty;
} Engine;

void engine_init(Engine *e, Snapshot *snap, const Config *cfg, const char *skip, emit_fn fn, void *ud);
void engine_free(Engine *e);
void engine_reset_counters(Engine *e);
void engine_begin(Engine *e);
void engine_item(Engine *e, const char *path, Record *nw, int hstat, int err);
void engine_end(Engine *e);
int  engine_hash(Engine *e, Record *nw, int *err);
int  engine_scan(Engine *e, const char *subtree, int deep);
void engine_reconcile(Engine *e, HashTab *paths);
void engine_worker_result(Engine *e, const char *path, const char *sha,
                          int64_t size, int64_t mtime_ns, int64_t ctime_ns);

int  signals_init(void);
int  signals_fd(void);
int  signals_next(void);
void signals_child_default(void);

typedef struct Request { char *id; char *reply; char *cmd; Vec args; } Request;
typedef struct Reply { Str buf; int is_err; } Reply;
typedef struct ControlIn { Str acc; int discarding; } ControlIn;

int  control_open(const char *fifo, int *rfd, int *wfd, Str *err);
int  control_read(int fd, ControlIn *in, void (*fn)(void *, const char *), void *ud);
int  control_parse(const char *line, Request *req, Str *err);
void request_free(Request *req);
void reply_init(Reply *r);
void reply_ok(Reply *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void reply_err(Reply *r, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void reply_free(Reply *r);
int  control_send(const char *reply_dir, const char *reply_path, Reply *r);

typedef struct Watcher {
    int      fd;
    HashTab *wd2dir;
    HashTab *dir2wd;
    int      overflow;
    int      limit;
} Watcher;

int    watcher_open(Watcher *w);
void   watcher_close(Watcher *w);
int    watcher_add_tree(Watcher *w, const ScanCtx *ctx, const char *root, const char *start, HashTab *pending);
void   watcher_remove_tree(Watcher *w, const char *dir);
int    watcher_has(const Watcher *w, const char *path);
size_t watcher_count(const Watcher *w);
int    watcher_read(Watcher *w, const ScanCtx *ctx, const Vec *roots, HashTab *pending);

int  daemonize(const char *out_path);
int  pidfile_check(const char *path);
int  pidfile_write(const char *path);
pid_t pidfile_read(const char *path);

typedef struct Worker {
    pid_t   pid;
    int     fd;
    Str     buf;
    int     done_seen, eof, reaped, status;
    size_t  count;
    int64_t start_ms;
} Worker;
typedef void (*worker_line_fn)(void *ud, const char *path, const char *sha,
                               int64_t size, int64_t mtime_ns, int64_t ctime_ns);
void worker_init(Worker *w);
int  worker_start(Worker *w, const Snapshot *s, int64_t max_bytes, const int *close_fds, int nclose);
int  worker_read(Worker *w, worker_line_fn fn, void *ud);
int  worker_running(const Worker *w);

typedef struct Hook {
    char   *path;
    int     min_sev;
    int     rate;
    int64_t win_start;
    int     count, dropped;
} Hook;
int  hook_validate(const char *path, Str *why);
void hook_set(Hook *h, const char *path, int min_sev, int rate);
int  hook_fire(Hook *h, const char *event, const char *path, int sev, uint64_t seq, const char *detail);
int  hook_take_dropped(Hook *h, int64_t now);

typedef struct DaemonOpts {
    char *rundir;
    char *config;
    Vec   watch;
    int   mode;
    int   foreground;
} DaemonOpts;
int daemon_main(DaemonOpts *o);

#endif
