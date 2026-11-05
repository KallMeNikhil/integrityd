#include "integrityd.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static struct {
    DaemonOpts *opts;
    Config      cfg;
    char       *cfg_path;
    char       *rundir, *snap_path, *log_path, *pid_path, *out_path, *fifo_path, *reply_dir;
    int         ctl_rfd, ctl_wfd;
    ControlIn   ctl_in;
    Snapshot    snap;
    Logger      log;
    int         log_open;
    Engine      eng;
    Watcher     w;
    int         watching;
    HashTab    *pending;
    int64_t     batch_first, batch_deadline;
    int64_t     flush_deadline;
    int         paused, shutdown, pid_written;
    int64_t     start_mono;
    unsigned long ticks;
    char        last_scan[32];
    int64_t     last_scan_ms;
    uint64_t    alerts_total;
    HashTab    *degraded;
    Worker      worker;
    size_t      worker_changes;
    Hook        hook;
    int         hook_internal;
} D;

static void emit(void *ud, int sev, const char *event, const char *path, const char *detail)
{
    (void)ud;
    if (!D.log_open) return;
    logger_write(&D.log, sev, event, path, detail);
    if (sev == SEV_ALERT) D.alerts_total++;
    if (!D.hook_internal) hook_fire(&D.hook, event, path, sev, D.log.seq, detail);
}

static void log_info(const char *event, const char *path, const char *detail)
{
    emit(NULL, SEV_INFO, event, path, detail);
}

static void log_error(const char *what, int err, const char *extra)
{
    Str d;
    str_init(&d);
    detail_add(&d, "what", what);
    if (err) detail_addf(&d, "errno", "%d", err);
    if (err) detail_add(&d, "error", strerror(err));
    if (extra && *extra) {
        str_appendc(&d, ';');
        str_append(&d, extra);
    }
    fprintf(stderr, "integrityd: ERROR %s\n", d.p);
    emit(NULL, SEV_WARN, "ERROR", NULL, d.p);
    str_free(&d);
}

static void save_snapshot(void)
{
    Str err;
    str_init(&err);
    if (snapshot_save(&D.snap, D.snap_path, &err) < 0) {
        Str x;
        str_init(&x);
        detail_add(&x, "file", "snapshot");
        log_error("io", errno, x.p);
        str_free(&x);
    } else {
        D.eng.dirty = 0;
    }
    D.flush_deadline = 0;
    str_free(&err);
}

static void schedule_flush(void)
{
    if (!D.eng.dirty) return;
    if (D.cfg.snapshot_flush_secs == 0) save_snapshot();
    else if (!D.flush_deadline) D.flush_deadline = now_ms() + (int64_t)D.cfg.snapshot_flush_secs * 1000;
}

static void run_scan(const char *kind, const char *subtree, int deep)
{
    int64_t t0 = now_ms();
    engine_reset_counters(&D.eng);
    engine_scan(&D.eng, subtree, deep);
    D.last_scan_ms = now_ms() - t0;
    fmt_utc(D.last_scan, time(NULL));
    Str d;
    str_init(&d);
    detail_add(&d, "kind", kind);
    if (subtree) detail_add(&d, "path", subtree);
    detail_addf(&d, "files", "%zu", D.eng.n_files);
    detail_addf(&d, "changes", "%zu", D.eng.n_changes);
    detail_addf(&d, "ms", "%lld", (long long)D.last_scan_ms);
    if (D.eng.extra) detail_add(&d, D.eng.extra, "1");
    log_info("SCAN_END", NULL, d.p);
    str_free(&d);
    if (D.eng.dirty) save_snapshot();
}

static void on_worker_line(void *ud, const char *path, const char *sha, int64_t size, int64_t mtime_ns,
                           int64_t ctime_ns)
{
    (void)ud;
    if (!D.paused) engine_worker_result(&D.eng, path, sha, size, mtime_ns, ctime_ns);
}

static void finish_worker(void)
{
    Worker *w = &D.worker;
    if (!w->eof || !w->reaped) return;
    int ok = w->done_seen && WIFEXITED(w->status) && WEXITSTATUS(w->status) == 0;
    Str d;
    str_init(&d);
    if (!ok) {
        detail_addf(&d, "pid", "%d", (int)w->pid);
        if (WIFSIGNALED(w->status)) detail_addf(&d, "signal", "%d", WTERMSIG(w->status));
        else detail_addf(&d, "exit", "%d", WEXITSTATUS(w->status));
        detail_add(&d, "done", w->done_seen ? "1" : "0");
        log_error("worker", 0, d.p);
    } else {
        D.last_scan_ms = now_ms() - w->start_ms;
        fmt_utc(D.last_scan, time(NULL));
        detail_add(&d, "kind", "deep");
        detail_add(&d, "worker", "1");
        detail_addf(&d, "files", "%zu", w->count);
        detail_addf(&d, "changes", "%zu", D.worker_changes);
        detail_addf(&d, "ms", "%lld", (long long)D.last_scan_ms);
        log_info("SCAN_END", NULL, d.p);
    }
    str_free(&d);
    w->pid = 0;
    if (D.eng.dirty) save_snapshot();
}

static void on_worker_readable(void)
{
    engine_reset_counters(&D.eng);
    engine_begin(&D.eng);
    int eof = worker_read(&D.worker, on_worker_line, NULL);
    engine_end(&D.eng);
    D.worker_changes += D.eng.n_changes;
    schedule_flush();
    if (eof) finish_worker();
}

static void reap_children(void)
{
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        if (pid == D.worker.pid) {
            D.worker.reaped = 1;
            D.worker.status = st;
            finish_worker();
        }
    }
}

static int run_deep(void)
{
    if (worker_running(&D.worker)) return 1;
    if (!D.cfg.deep_verify_in_worker || (int64_t)ht_count(D.snap.recs) <= D.cfg.worker_min_records) {
        run_scan("deep", NULL, 1);
        return 0;
    }
    run_scan("deep_meta", NULL, 0);
    int fds[4] = { D.ctl_rfd, D.ctl_wfd, D.watching ? D.w.fd : -1, D.log.fp ? fileno(D.log.fp) : -1 };
    D.worker_changes = 0;
    if (worker_start(&D.worker, &D.snap, D.cfg.hash_max_bytes, fds, 4) < 0) {
        log_error("worker_fork", errno, NULL);
        run_scan("deep", NULL, 1);
        return 0;
    }
    return 2;
}

static void configure_hook(void)
{
    hook_set(&D.hook, NULL, D.cfg.hook_min_severity, D.cfg.hook_rate_per_sec);
    if (!D.cfg.alert_hook) return;
    Str why;
    str_init(&why);
    if (hook_validate(D.cfg.alert_hook, &why) == 0) {
        hook_set(&D.hook, D.cfg.alert_hook, D.cfg.hook_min_severity, D.cfg.hook_rate_per_sec);
    } else {
        Str x;
        str_init(&x);
        detail_add(&x, "path", D.cfg.alert_hook);
        detail_add(&x, "why", why.p);
        D.hook_internal = 1;
        log_error("hook_rejected", 0, x.p);
        D.hook_internal = 0;
        str_free(&x);
    }
    str_free(&why);
}

static void adopt(const char *subtree)
{
    D.eng.silent = 1;
    engine_reset_counters(&D.eng);
    engine_scan(&D.eng, subtree, 1);
    D.eng.silent = 0;
}

static void watch_root(const char *root, HashTab *pending)
{
    if (!D.watching || watcher_has(&D.w, root)) return;
    if (access(root, F_OK) != 0) return;
    ScanCtx ctx = { &D.cfg, D.rundir };
    if (watcher_add_tree(&D.w, &ctx, root, root, pending) < 0 && !ht_has(D.degraded, root)) {
        ht_put(D.degraded, root, NULL);
        Str x;
        str_init(&x);
        detail_add(&x, "root", root);
        detail_add(&x, "fallback", "scan");
        log_error("watch_limit", ENOSPC, x.p);
        str_free(&x);
    }
}

static void watch_all(HashTab *pending)
{
    for (size_t i = 0; i < D.snap.roots.n; i++) watch_root(D.snap.roots.items[i], pending);
}

static void watcher_start(void)
{
    if (D.watching || D.cfg.mode == MODE_SCAN) return;
    if (watcher_open(&D.w) < 0) {
        log_error("inotify_init", errno, "fallback=scan");
        return;
    }
    D.watching = 1;
    watch_all(NULL);
}

static void watcher_stop(void)
{
    if (!D.watching) return;
    watcher_close(&D.w);
    D.watching = 0;
}

static void process_batch(void)
{
    HashTab *paths = D.pending;
    D.pending = ht_new();
    D.batch_first = 0;
    engine_reset_counters(&D.eng);
    engine_reconcile(&D.eng, paths);
    ht_free(paths, NULL);
    watch_all(D.pending);
    if (ht_count(D.pending)) {
        D.batch_first = now_ms();
        D.batch_deadline = D.batch_first + D.cfg.batch_ms;
    }
    schedule_flush();
}

static void on_inotify(void)
{
    ScanCtx ctx = { &D.cfg, D.rundir };
    int n = watcher_read(&D.w, &ctx, &D.snap.roots, D.pending);
    if (n < 0) log_error("inotify_read", errno, NULL);
    if (D.w.overflow) {
        D.w.overflow = 0;
        log_info("OVERFLOW_RESCAN", NULL, NULL);
        if (!D.paused) run_scan("overflow", NULL, 0);
    }
    if (n > 0 && ht_count(D.pending)) {
        int64_t now = now_ms();
        if (!D.batch_first) D.batch_first = now;
        D.batch_deadline = now + D.cfg.batch_ms;
        if (D.batch_deadline > D.batch_first + D.cfg.batch_max_ms) D.batch_deadline = D.batch_first + D.cfg.batch_max_ms;
    }
}

static void on_tick(void)
{
    if (!D.paused) {
        D.ticks++;
        if (D.cfg.mode != MODE_WATCH) {
            if (D.ticks % (unsigned long)D.cfg.deep_every == 0) run_deep();
            else run_scan("meta", NULL, 0);
        } else {
            Vec roots;
            vec_init(&roots);
            ht_keys(D.degraded, &roots);
            for (size_t i = 0; i < roots.n; i++) run_scan("degraded", roots.items[i], 0);
            vec_free(&roots);
        }
        watch_all(D.cfg.mode == MODE_WATCH ? D.pending : NULL);
    }
    alarm((unsigned)D.cfg.scan_interval);
}

static void status_line(Str *s, char sep)
{
    str_appendf(s, "state=%s%cmode=%s%croots=%zu%crecords=%zu%cwatches=%zu%cuptime_s=%lld%c"
                   "last_scan=%s%clast_scan_ms=%lld%calerts_total=%llu%clog_seq=%llu%cpid=%d%cworker=%s",
                D.paused ? "paused" : "running", sep, mode_name(D.cfg.mode), sep, D.snap.roots.n, sep,
                ht_count(D.snap.recs), sep, D.watching ? watcher_count(&D.w) : (size_t)0, sep,
                (long long)((now_ms() - D.start_mono) / 1000), sep, D.last_scan, sep, (long long)D.last_scan_ms,
                sep, (unsigned long long)D.alerts_total, sep, (unsigned long long)D.log.seq, sep, (int)getpid(),
                sep, worker_running(&D.worker) ? "running" : "idle");
}

static int do_reload(Str *why)
{
    Config c;
    config_init(&c);
    if (D.cfg_path) {
        Str err;
        str_init(&err);
        if (config_load(&c, D.cfg_path, &err) < 0) {
            str_append(why, err.p);
            Str x;
            str_init(&x);
            detail_add(&x, "why", err.p);
            log_error("config_reload", 0, x.p);
            str_free(&x);
            str_free(&err);
            config_free(&c);
            return -1;
        }
        str_free(&err);
    }
    if (D.opts->mode >= 0) c.mode = D.opts->mode;
    config_free(&D.cfg);
    D.cfg = c;
    D.log.do_fsync = D.cfg.log_fsync;
    D.log.max_bytes = D.cfg.log_max_bytes;
    D.log.keep = D.cfg.log_keep;
    if (D.cfg.mode == MODE_SCAN) watcher_stop();
    else watcher_start();
    configure_hook();
    alarm((unsigned)D.cfg.scan_interval);
    Str d;
    str_init(&d);
    detail_add(&d, "mode", mode_name(D.cfg.mode));
    detail_addf(&d, "scan_interval", "%d", D.cfg.scan_interval);
    detail_addf(&d, "excludes", "%zu", D.cfg.exclude.n);
    log_info("CONFIG_RELOAD", NULL, d.p);
    str_free(&d);
    return 0;
}

static void handle_signal(int sig)
{
    switch (sig) {
    case SIGTERM:
    case SIGINT: D.shutdown = 1; break;
    case SIGALRM: on_tick(); break;
    case SIGUSR1:
        if (!D.paused) run_scan("signal", NULL, 0);
        break;
    case SIGUSR2: {
        Str s;
        str_init(&s);
        status_line(&s, ';');
        log_info("STATUS", NULL, s.p);
        str_free(&s);
        break;
    }
    case SIGHUP: {
        Str why;
        str_init(&why);
        do_reload(&why);
        str_free(&why);
        break;
    }
    case SIGCHLD: reap_children(); break;
    default: break;
    }
}

static char *canon_arg(const char *arg)
{
    if (arg[0] != '/') return NULL;
    char *real = realpath(arg, NULL);
    if (real) return real;
    char *p = xstrdup(arg);
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = '\0';
    return p;
}

static void cmd_pause(Reply *r)
{
    if (D.paused) {
        reply_ok(r, "already paused");
        return;
    }
    D.paused = 1;
    log_info("PAUSED", NULL, NULL);
    reply_ok(r, "paused");
}

static void cmd_resume(Reply *r)
{
    if (!D.paused) {
        reply_ok(r, "not paused");
        return;
    }
    D.paused = 0;
    log_info("RESUMED", NULL, NULL);
    D.eng.extra = "during_pause";
    size_t changes = 0;
    if (ht_count(D.pending)) {
        process_batch();
        changes += D.eng.n_changes;
    }
    run_scan("resume", NULL, 0);
    changes += D.eng.n_changes;
    D.eng.extra = NULL;
    reply_ok(r, "resumed changes=%zu", changes);
}

static void cmd_add(Reply *r, const char *arg)
{
    char *path = canon_arg(arg);
    struct stat st;
    if (!path || lstat(path, &st) < 0) {
        reply_err(r, 422, "%s: no such file or directory", arg);
        free(path);
        return;
    }
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
        reply_err(r, 422, "%s: not a directory or regular file", path);
        free(path);
        return;
    }
    const char *covering = snapshot_root_of(&D.snap, path);
    if (covering) {
        reply_err(r, 422, "already covered by root %s", covering);
        free(path);
        return;
    }
    if (path_under(path, D.rundir)) {
        reply_err(r, 422, "refusing to watch the run directory");
        free(path);
        return;
    }
    size_t merged = 0;
    for (size_t i = 0; i < D.snap.roots.n; i++) merged += path_under(D.snap.roots.items[i], path);
    snapshot_add_root(&D.snap, path);
    adopt(path);
    watch_root(path, NULL);
    save_snapshot();
    Str d;
    str_init(&d);
    detail_addf(&d, "records", "%zu", D.eng.n_added + D.eng.n_changed);
    detail_addf(&d, "merged", "%zu", merged);
    log_info("WATCH_ADDED", path, d.p);
    str_free(&d);
    reply_ok(r, "added merged=%zu", merged);
    free(path);
}

static void cmd_remove(Reply *r, const char *arg)
{
    char *path = canon_arg(arg);
    if (!path || snapshot_remove_root(&D.snap, path) < 0) {
        reply_err(r, 422, "%s: not a watched root", arg);
        free(path);
        return;
    }
    size_t dropped = 0;
    snapshot_drop_under(&D.snap, path, &dropped);
    if (D.watching) watcher_remove_tree(&D.w, path);
    free(ht_del(D.degraded, path));
    Vec keys;
    vec_init(&keys);
    ht_keys(D.pending, &keys);
    for (size_t i = 0; i < keys.n; i++)
        if (path_under(keys.items[i], path)) ht_del(D.pending, keys.items[i]);
    vec_free(&keys);
    save_snapshot();
    Str d;
    str_init(&d);
    detail_addf(&d, "records", "%zu", dropped);
    log_info("WATCH_REMOVED", path, d.p);
    str_free(&d);
    reply_ok(r, "removed records=%zu", dropped);
    free(path);
}

static void cmd_rebaseline(Reply *r, const char *arg)
{
    char *path = arg ? canon_arg(arg) : NULL;
    if (arg && (!path || !snapshot_root_of(&D.snap, path))) {
        reply_err(r, 422, "%s: not inside a watched root", arg);
        free(path);
        return;
    }
    adopt(path);
    save_snapshot();
    Str d;
    str_init(&d);
    if (path) detail_add(&d, "path", path);
    detail_addf(&d, "added", "%zu", D.eng.n_added);
    detail_addf(&d, "removed", "%zu", D.eng.n_removed);
    detail_addf(&d, "changed", "%zu", D.eng.n_changed);
    detail_addf(&d, "records", "%zu", ht_count(D.snap.recs));
    log_info("REBASELINE", NULL, d.p);
    reply_ok(r, "%s", d.p);
    str_free(&d);
    free(path);
}

static void cmd_tail(Reply *r, const char *arg)
{
    long n = 20;
    if (arg) {
        char *end;
        n = strtol(arg, &end, 10);
        if (*end || n < 1 || n > 200) {
            reply_err(r, 422, "TAIL n: n must be 1..200");
            return;
        }
    }
    Vec lines;
    vec_init(&lines);
    log_tail(D.log_path, (int)n, &lines);
    for (size_t i = 0; i < lines.n; i++) reply_ok(r, "%s", (char *)lines.items[i]);
    if (lines.n == 0) reply_ok(r, "(empty log)");
    vec_free_all(&lines, free);
}

static void handle_command(void *ud, const char *line)
{
    (void)ud;
    Request q;
    Str err;
    str_init(&err);
    Reply r;
    reply_init(&r);
    int ok = control_parse(line, &q, &err) == 0;
    const char *arg = q.args.n ? q.args.items[0] : NULL;
    const char *c = q.cmd ? q.cmd : "";
    if (!ok) reply_err(&r, 400, "malformed request: %s", err.p);
    else if (!strcmp(c, "PING")) reply_ok(&r, "pong");
    else if (!strcmp(c, "VERSION")) reply_ok(&r, "%s", INTEGRITYD_VERSION);
    else if (!strcmp(c, "STATUS")) {
        Str s;
        str_init(&s);
        status_line(&s, ' ');
        reply_ok(&r, "%s", s.p);
        str_free(&s);
    } else if (!strcmp(c, "PAUSE")) cmd_pause(&r);
    else if (!strcmp(c, "RESUME")) cmd_resume(&r);
    else if (!strcmp(c, "DEEPVERIFY")) {
        if (D.paused) reply_err(&r, 409, "paused (RESUME performs a full scan)");
        else {
            int how = run_deep();
            if (how == 1) reply_err(&r, 409, "a deep-verify worker is already running (pid %d)", (int)D.worker.pid);
            else if (how == 2) reply_ok(&r, "worker started pid=%d", (int)D.worker.pid);
            else reply_ok(&r, "files=%zu changes=%zu ms=%lld", D.eng.n_files, D.eng.n_changes, (long long)D.last_scan_ms);
        }
    } else if (!strcmp(c, "RESCAN")) {
        char *path = arg ? canon_arg(arg) : NULL;
        if (D.paused) reply_err(&r, 409, "paused (RESUME performs a full scan)");
        else if (arg && (!path || !snapshot_root_of(&D.snap, path)))
            reply_err(&r, 422, "%s: not inside a watched root", arg);
        else {
            run_scan("rescan", path, 0);
            reply_ok(&r, "files=%zu changes=%zu ms=%lld", D.eng.n_files, D.eng.n_changes, (long long)D.last_scan_ms);
        }
        free(path);
    } else if (!strcmp(c, "ADD") || !strcmp(c, "REMOVE")) {
        if (!arg) reply_err(&r, 422, "%s needs an absolute path", c);
        else if (!strcmp(c, "ADD")) cmd_add(&r, arg);
        else cmd_remove(&r, arg);
    } else if (!strcmp(c, "LIST")) {
        for (size_t i = 0; i < D.snap.roots.n; i++) {
            Str e;
            str_init(&e);
            escape_append(&e, D.snap.roots.items[i], ESC_ARG);
            reply_ok(&r, "%s", e.p);
            str_free(&e);
        }
        if (D.snap.roots.n == 0) reply_ok(&r, "(no roots)");
    } else if (!strcmp(c, "REBASELINE")) cmd_rebaseline(&r, arg);
    else if (!strcmp(c, "RELOAD")) {
        Str why;
        str_init(&why);
        if (do_reload(&why) == 0) reply_ok(&r, "reloaded mode=%s", mode_name(D.cfg.mode));
        else reply_err(&r, 422, "config error: %s", why.p);
        str_free(&why);
    } else if (!strcmp(c, "TAIL")) cmd_tail(&r, arg);
    else if (!strcmp(c, "SHUTDOWN")) {
        reply_ok(&r, "shutting down");
        D.shutdown = 1;
    } else reply_err(&r, 400, "unknown command");
    control_send(D.reply_dir, q.reply, &r);
    reply_free(&r);
    request_free(&q);
    str_free(&err);
}

static int compute_timeout(void)
{
    int64_t now = now_ms(), deadline = -1;
    if (ht_count(D.pending) && !D.paused) deadline = D.batch_deadline;
    if (D.flush_deadline && (deadline < 0 || D.flush_deadline < deadline)) deadline = D.flush_deadline;
    if (D.hook.dropped) {
        int64_t h = D.hook.win_start + 1000;
        if (deadline < 0 || h < deadline) deadline = h;
    }
    if (deadline < 0) return -1;
    return deadline <= now ? 0 : (int)(deadline - now);
}

static void report_hook_drops(void)
{
    int dropped = hook_take_dropped(&D.hook, now_ms());
    if (!dropped) return;
    Str x;
    str_init(&x);
    detail_addf(&x, "hook_dropped", "%d", dropped);
    D.hook_internal = 1;
    log_error("hook_rate", 0, x.p);
    D.hook_internal = 0;
    str_free(&x);
}

static void event_loop(void)
{
    while (!D.shutdown) {
        struct pollfd pfd[4];
        int n = 0, ip = -1, cp = -1, wp = -1;
        pfd[n].fd = signals_fd();
        pfd[n++].events = POLLIN;
        if (D.ctl_rfd >= 0) {
            cp = n;
            pfd[n].fd = D.ctl_rfd;
            pfd[n++].events = POLLIN;
        }
        if (D.watching) {
            ip = n;
            pfd[n].fd = D.w.fd;
            pfd[n++].events = POLLIN;
        }
        if (D.worker.fd >= 0) {
            wp = n;
            pfd[n].fd = D.worker.fd;
            pfd[n++].events = POLLIN;
        }
        int r = poll(pfd, (nfds_t)n, compute_timeout());
        if (r < 0 && errno != EINTR) {
            log_error("poll", errno, NULL);
            break;
        }
        if (r > 0 && (pfd[0].revents & POLLIN)) {
            int sig;
            while ((sig = signals_next()) != 0) handle_signal(sig);
        }
        if (r > 0 && cp >= 0 && (pfd[cp].revents & POLLIN)) control_read(D.ctl_rfd, &D.ctl_in, handle_command, NULL);
        if (r > 0 && ip >= 0 && D.watching && (pfd[ip].revents & POLLIN)) on_inotify();
        if (r > 0 && wp >= 0 && D.worker.fd >= 0 && (pfd[wp].revents & (POLLIN | POLLHUP | POLLERR)))
            on_worker_readable();
        int64_t now = now_ms();
        if (ht_count(D.pending) && !D.paused && now >= D.batch_deadline) process_batch();
        if (D.flush_deadline && now >= D.flush_deadline) save_snapshot();
        report_hook_drops();
    }
}

static int load_config(void)
{
    config_init(&D.cfg);
    if (D.opts->config) D.cfg_path = xstrdup(D.opts->config);
    else {
        char *p = path_join(D.rundir, "integrityd.conf");
        if (access(p, F_OK) == 0) D.cfg_path = p;
        else free(p);
    }
    if (D.cfg_path) {
        Str err;
        str_init(&err);
        int rc = config_load(&D.cfg, D.cfg_path, &err);
        if (rc < 0) fprintf(stderr, "integrityd: config errors:\n%s", err.p);
        str_free(&err);
        if (rc < 0) return -1;
    }
    for (size_t i = 0; i < D.opts->watch.n; i++) vec_push(&D.cfg.watch, xstrdup(D.opts->watch.items[i]));
    if (D.opts->mode >= 0) D.cfg.mode = D.opts->mode;
    return 0;
}

static void startup_snapshot(void)
{
    Str err;
    str_init(&err);
    int st = snapshot_load(&D.snap, D.snap_path, &err);
    if (st == SNAP_CORRUPT || st == SNAP_IOERR) {
        Str moved, x;
        str_init(&moved);
        str_init(&x);
        str_appendf(&moved, "%s.corrupt.%lld", D.snap_path, (long long)time(NULL));
        rename(D.snap_path, moved.p);
        detail_add(&x, "why", err.p);
        detail_add(&x, "moved_to", moved.p);
        log_error("snapshot_corrupt", 0, x.p);
        str_free(&moved);
        str_free(&x);
        snapshot_free(&D.snap);
        snapshot_init(&D.snap);
        st = SNAP_MISSING;
    }
    str_free(&err);
    Str d;
    str_init(&d);
    if (st == SNAP_MISSING) {
        for (size_t i = 0; i < D.cfg.watch.n; i++) snapshot_add_root(&D.snap, D.cfg.watch.items[i]);
        if (D.snap.roots.n == 0) warnmsg("no roots to watch yet (add one with: integrityctl add DIR)");
        adopt(NULL);
        save_snapshot();
        detail_add(&d, "baseline", "initial");
    } else {
        detail_add(&d, "baseline", "loaded");
    }
    detail_addf(&d, "records", "%zu", ht_count(D.snap.recs));
    detail_addf(&d, "roots", "%zu", D.snap.roots.n);
    detail_add(&d, "mode", mode_name(D.cfg.mode));
    detail_addf(&d, "pid", "%d", (int)getpid());
    log_info("DAEMON_START", NULL, d.p);
    str_free(&d);
    if (st == SNAP_MISSING) return;

    for (size_t i = 0; i < D.cfg.watch.n; i++) {
        const char *root = D.cfg.watch.items[i];
        if (snapshot_add_root(&D.snap, root) != 0) continue;
        adopt(root);
        log_info("WATCH_ADDED", root, "source=config");
        save_snapshot();
    }
    if (D.cfg.startup_check != STARTUP_NONE) {
        D.eng.extra = "offline";
        run_scan(D.cfg.startup_check == STARTUP_DEEP ? "startup_deep" : "startup_meta", NULL,
                 D.cfg.startup_check == STARTUP_DEEP);
        D.eng.extra = NULL;
    }
}

static void cleanup(void)
{
    if (worker_running(&D.worker)) {
        kill(D.worker.pid, SIGTERM);
        waitpid(D.worker.pid, NULL, 0);
        D.worker.pid = 0;
    }
    if (D.worker.fd >= 0) close(D.worker.fd);
    str_free(&D.worker.buf);
    hook_set(&D.hook, NULL, 0, 0);
    if (D.watching) watcher_stop();
    if (D.log_open) {
        save_snapshot();
        Str d;
        str_init(&d);
        detail_addf(&d, "uptime_s", "%lld", (long long)((now_ms() - D.start_mono) / 1000));
        detail_addf(&d, "alerts_total", "%llu", (unsigned long long)D.alerts_total);
        log_info("DAEMON_STOP", NULL, d.p);
        str_free(&d);
        logger_close(&D.log);
        D.log_open = 0;
    }
    if (D.ctl_rfd >= 0) {
        close(D.ctl_rfd);
        close(D.ctl_wfd);
        unlink(D.fifo_path);
    }
    str_free(&D.ctl_in.acc);
    if (D.pid_written) unlink(D.pid_path);
    engine_free(&D.eng);
    snapshot_free(&D.snap);
    ht_free(D.pending, NULL);
    ht_free(D.degraded, NULL);
    config_free(&D.cfg);
    free(D.cfg_path);
    free(D.snap_path);
    free(D.log_path);
    free(D.pid_path);
    free(D.out_path);
    free(D.fifo_path);
    free(D.reply_dir);
}

int daemon_main(DaemonOpts *o)
{
    memset(&D, 0, sizeof D);
    D.opts = o;
    D.rundir = o->rundir;
    D.start_mono = now_ms();
    D.snap_path = path_join(D.rundir, "snapshot.db");
    D.log_path = path_join(D.rundir, "change.log");
    D.pid_path = path_join(D.rundir, "integrityd.pid");
    D.out_path = path_join(D.rundir, "daemon.out");
    D.fifo_path = path_join(D.rundir, "control.fifo");
    D.reply_dir = path_join(D.rundir, "reply");
    D.ctl_rfd = D.ctl_wfd = -1;
    str_init(&D.ctl_in.acc);
    worker_init(&D.worker);
    D.pending = ht_new();
    D.degraded = ht_new();
    snapshot_init(&D.snap);
    Str err;
    str_init(&err);
    engine_init(&D.eng, &D.snap, &D.cfg, D.rundir, emit, NULL);
    strcpy(D.last_scan, "-");

    int rc = 0;
    if (mkdir_p(D.rundir, 0700) < 0 || chmod(D.rundir, 0700) < 0) {
        warnmsg("%s: %s", D.rundir, strerror(errno));
        rc = 1;
        goto out;
    }
    if (load_config() < 0) {
        rc = 2;
        goto out;
    }
    if (pidfile_check(D.pid_path) == 1) {
        warnmsg("already running (pid %d, run dir %s)", (int)pidfile_read(D.pid_path), D.rundir);
        rc = 3;
        goto out;
    }
    if (!o->foreground && daemonize(D.out_path) < 0) {
        warnmsg("daemonize: %s", strerror(errno));
        rc = 1;
        goto out;
    }
    if (pidfile_write(D.pid_path) < 0) {
        warnmsg("%s: %s (already running?)", D.pid_path, strerror(errno));
        rc = 3;
        goto out;
    }
    D.pid_written = 1;

    if (logger_open(&D.log, D.log_path, D.cfg.log_fsync, D.cfg.log_max_bytes, D.cfg.log_keep, &err) < 0) {
        warnmsg("%s", err.p);
        rc = 1;
        goto out;
    }
    str_clear(&err);
    D.log_open = 1;
    if (D.log.partial) {
        Str x;
        str_init(&x);
        detail_addf(&x, "bytes", "%lld", (long long)D.log.partial);
        log_error("log_partial_line", 0, x.p);
        str_free(&x);
    }

    startup_snapshot();

    if (signals_init() < 0) {
        log_error("signals", errno, NULL);
        rc = 1;
        goto out;
    }
    if (mkdir_p(D.reply_dir, 0700) < 0 || control_open(D.fifo_path, &D.ctl_rfd, &D.ctl_wfd, &err) < 0) {
        Str x;
        str_init(&x);
        detail_add(&x, "why", err.len ? err.p : strerror(errno));
        log_error("control_fifo", 0, x.p);
        str_free(&x);
        D.ctl_rfd = D.ctl_wfd = -1;
    }
    str_free(&err);
    configure_hook();
    watcher_start();
    alarm((unsigned)D.cfg.scan_interval);
    if (o->foreground) fprintf(stderr, "integrityd: ready (pid %d, mode %s)\n", (int)getpid(), mode_name(D.cfg.mode));

    event_loop();

    if (ht_count(D.pending) && !D.paused) process_batch();
out:
    str_free(&err);
    cleanup();
    return rc;
}
