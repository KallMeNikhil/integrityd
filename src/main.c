#include "integrityd.h"

#include <errno.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Opts {
    char *rundir, *config, *log;
    Vec   watch;
    int   mode, foreground, deep, update, scan_only;
} Opts;

static void usage(FILE *f)
{
    fputs("usage:\n"
          "  integrityd daemon   [--rundir D] [--config F] [--watch DIR ...] [--mode hybrid|scan|watch] [--foreground]\n"
          "  integrityd baseline --watch DIR ... [--rundir D] [--config F]\n"
          "  integrityd check    [--rundir D] [--config F] [--deep] [--update] [--scan-only] [--watch DIR ...]\n"
          "  integrityd hash     FILE...\n"
          "  integrityd verify-log [--rundir D | --log FILE]\n"
          "  integrityd version\n",
          f);
}

static int parse_opts(int argc, char **argv, Opts *o)
{
    static const struct option longopts[] = {
        { "rundir", required_argument, NULL, 'r' },  { "config", required_argument, NULL, 'c' },
        { "watch", required_argument, NULL, 'w' },   { "mode", required_argument, NULL, 'm' },
        { "foreground", no_argument, NULL, 'f' },    { "deep", no_argument, NULL, 'd' },
        { "update", no_argument, NULL, 'u' },        { "scan-only", no_argument, NULL, 's' },
        { "log", required_argument, NULL, 'l' },     { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    memset(o, 0, sizeof *o);
    vec_init(&o->watch);
    o->mode = -1;
    optind = 1;
    int c;
    while ((c = getopt_long(argc, argv, "r:c:w:m:fdusl:h", longopts, NULL)) != -1) {
        switch (c) {
        case 'r': free(o->rundir); o->rundir = xstrdup(optarg); break;
        case 'c': free(o->config); o->config = xstrdup(optarg); break;
        case 'l': free(o->log); o->log = xstrdup(optarg); break;
        case 'w': {
            char *real = realpath(optarg, NULL);
            if (!real) {
                warnmsg("--watch %s: %s", optarg, strerror(errno));
                return 2;
            }
            vec_push(&o->watch, real);
            break;
        }
        case 'm':
            if ((o->mode = parse_mode(optarg)) < 0) {
                warnmsg("--mode: invalid value '%s'", optarg);
                return 2;
            }
            break;
        case 'f': o->foreground = 1; break;
        case 'd': o->deep = 1; break;
        case 'u': o->update = 1; break;
        case 's': o->scan_only = 1; break;
        case 'h': usage(stdout); exit(0);
        default: usage(stderr); return 2;
        }
    }
    if (!o->rundir) o->rundir = default_rundir();
    if (o->rundir[0] != '/') {
        char *cwd = getcwd(NULL, 0);
        if (!cwd) {
            warnmsg("getcwd: %s", strerror(errno));
            return 2;
        }
        char *abs = path_join(cwd, o->rundir);
        free(cwd);
        free(o->rundir);
        o->rundir = abs;
    }
    return 0;
}

static void opts_free(Opts *o)
{
    free(o->rundir);
    free(o->config);
    free(o->log);
    vec_free_all(&o->watch, free);
}

static int load_config(const Opts *o, Config *cfg)
{
    config_init(cfg);
    char *path = o->config ? xstrdup(o->config) : path_join(o->rundir, "integrityd.conf");
    int rc = 0;
    if (o->config || access(path, F_OK) == 0) {
        Str err;
        str_init(&err);
        if (config_load(cfg, path, &err) < 0) {
            fprintf(stderr, "integrityd: config errors:\n%s", err.p);
            rc = -1;
        }
        str_free(&err);
    }
    free(path);
    for (size_t i = 0; i < o->watch.n; i++) vec_push(&cfg->watch, xstrdup(o->watch.items[i]));
    if (o->mode >= 0) cfg->mode = o->mode;
    return rc;
}

static int cmd_scan_only(const Opts *o, const Config *cfg)
{
    ScanCtx ctx = { cfg, o->rundir };
    int rc = 0;
    if (cfg->watch.n == 0) {
        warnmsg("no roots given (use --watch DIR)");
        return 2;
    }
    for (size_t i = 0; i < cfg->watch.n; i++) {
        ScanResult res;
        scan_result_init(&res);
        if (scan_tree(&ctx, cfg->watch.items[i], cfg->watch.items[i], &res) < 0) {
            warnmsg("%s: %s", (char *)cfg->watch.items[i], strerror(errno));
            rc = 2;
        }
        Str line;
        str_init(&line);
        for (size_t j = 0; j < res.recs.n; j++) {
            str_clear(&line);
            record_format(&line, res.recs.items[j]);
            puts(line.p);
        }
        for (size_t j = 0; j < res.unreadable.n; j++)
            warnmsg("unreadable: %s (%s)", (char *)res.unreadable.items[j],
                    strerror((int)(intptr_t)res.errs.items[j]));
        str_free(&line);
        scan_result_free(&res);
    }
    return rc;
}

typedef struct CheckOut {
    Logger  out;
    Logger *file;
} CheckOut;

static void check_emit(void *ud, int sev, const char *event, const char *path, const char *detail)
{
    CheckOut *co = ud;
    logger_write(&co->out, sev, event, path, detail);
    if (co->file) logger_write(co->file, sev, event, path, detail);
}

static int open_log(const char *rundir, const Config *cfg, Logger *l)
{
    char *path = path_join(rundir, "change.log");
    Str err;
    str_init(&err);
    int rc = logger_open(l, path, cfg->log_fsync, cfg->log_max_bytes, cfg->log_keep, &err);
    if (rc < 0) warnmsg("%s", err.p);
    else if (l->partial)
        logger_write(l, SEV_WARN, "ERROR", NULL, "what=log_partial_line");
    str_free(&err);
    free(path);
    return rc;
}

static int daemon_running(const char *rundir)
{
    char *pid = path_join(rundir, "integrityd.pid");
    int running = pidfile_check(pid) == 1;
    free(pid);
    if (running) warnmsg("a daemon is running on %s; use integrityctl instead", rundir);
    return running;
}

static int cmd_baseline(Opts *o)
{
    Config cfg;
    if (load_config(o, &cfg) < 0) {
        config_free(&cfg);
        return 2;
    }
    if (cfg.watch.n == 0) {
        warnmsg("baseline: no roots given (use --watch DIR)");
        config_free(&cfg);
        return 2;
    }
    if (mkdir_p(o->rundir, 0700) < 0) {
        warnmsg("%s: %s", o->rundir, strerror(errno));
        config_free(&cfg);
        return 1;
    }
    if (daemon_running(o->rundir)) {
        config_free(&cfg);
        return 3;
    }
    Snapshot snap;
    snapshot_init(&snap);
    for (size_t i = 0; i < cfg.watch.n; i++) snapshot_add_root(&snap, cfg.watch.items[i]);
    Engine eng;
    engine_init(&eng, &snap, &cfg, o->rundir, NULL, NULL);
    eng.silent = 1;
    engine_scan(&eng, NULL, 1);
    char *path = path_join(o->rundir, "snapshot.db");
    Str err;
    str_init(&err);
    int rc = 0;
    if (snapshot_save(&snap, path, &err) < 0) {
        warnmsg("%s", err.p);
        rc = 1;
    } else {
        Logger log;
        if (open_log(o->rundir, &cfg, &log) == 0) {
            Str d;
            str_init(&d);
            detail_add(&d, "source", "baseline");
            detail_addf(&d, "records", "%zu", ht_count(snap.recs));
            detail_addf(&d, "roots", "%zu", snap.roots.n);
            logger_write(&log, SEV_INFO, "REBASELINE", NULL, d.p);
            str_free(&d);
            logger_close(&log);
        }
        printf("baseline: %zu records in %zu roots -> %s\n", ht_count(snap.recs), snap.roots.n, path);
    }
    str_free(&err);
    free(path);
    engine_free(&eng);
    snapshot_free(&snap);
    config_free(&cfg);
    return rc;
}

static int cmd_check(Opts *o)
{
    Config cfg;
    if (load_config(o, &cfg) < 0) {
        config_free(&cfg);
        return 2;
    }
    Snapshot snap;
    snapshot_init(&snap);
    char *path = path_join(o->rundir, "snapshot.db");
    Str err;
    str_init(&err);
    int rc = 2;
    int st = snapshot_load(&snap, path, &err);
    if (o->scan_only) {
        if (cfg.watch.n == 0 && st == SNAP_OK)
            for (size_t i = 0; i < snap.roots.n; i++) vec_push(&cfg.watch, xstrdup(snap.roots.items[i]));
        rc = cmd_scan_only(o, &cfg);
        goto out;
    }
    if (st == SNAP_MISSING) {
        warnmsg("no snapshot at %s (run 'integrityd baseline --watch DIR' first)", path);
        goto out;
    }
    if (st != SNAP_OK) {
        warnmsg("%s", err.p);
        rc = st == SNAP_CORRUPT ? 4 : 2;
        goto out;
    }
    if (o->update && daemon_running(o->rundir)) {
        rc = 3;
        goto out;
    }
    CheckOut co;
    Logger file;
    logger_stream(&co.out, stdout);
    co.file = NULL;
    if (o->update) {
        if (open_log(o->rundir, &cfg, &file) < 0) goto out;
        co.file = &file;
    }
    Engine eng;
    engine_init(&eng, &snap, &cfg, o->rundir, check_emit, &co);
    engine_scan(&eng, NULL, o->deep);
    rc = eng.n_changes ? 1 : 0;
    if (o->update && eng.dirty && snapshot_save(&snap, path, &err) < 0) {
        warnmsg("%s", err.p);
        rc = 2;
    }
    if (co.file) logger_close(co.file);
    engine_free(&eng);
out:
    str_free(&err);
    free(path);
    snapshot_free(&snap);
    config_free(&cfg);
    return rc;
}

static int cmd_daemon(Opts *o)
{
    DaemonOpts d;
    d.rundir = o->rundir;
    d.config = o->config;
    d.watch = o->watch;
    d.mode = o->mode;
    d.foreground = o->foreground;
    return daemon_main(&d);
}

static int cmd_verify_log(Opts *o)
{
    char *path = o->log ? xstrdup(o->log) : path_join(o->rundir, "change.log");
    int rc = verify_log(path, stdout);
    free(path);
    return rc;
}

static int cmd_hash(int argc, char **argv)
{
    int rc = 0;
    if (argc < 1) {
        usage(stderr);
        return 2;
    }
    for (int i = 0; i < argc; i++) {
        char hex[65];
        int err = 0;
        int st = hash_file(argv[i], 0, hex, NULL, &err);
        if (st == HASH_OK) {
            printf("%s  %s\n", hex, argv[i]);
        } else {
            warnmsg("%s: %s%s%s", argv[i], hash_status_name(st), err ? ": " : "", err ? strerror(err) : "");
            rc = 1;
        }
    }
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        puts(INTEGRITYD_VERSION);
        return 0;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "hash") == 0) return cmd_hash(argc - 2, argv + 2);
    Opts o;
    int rc = parse_opts(argc - 1, argv + 1, &o);
    if (rc == 0) {
        if (strcmp(cmd, "daemon") == 0) rc = cmd_daemon(&o);
        else if (strcmp(cmd, "check") == 0) rc = cmd_check(&o);
        else if (strcmp(cmd, "baseline") == 0) rc = cmd_baseline(&o);
        else if (strcmp(cmd, "verify-log") == 0) rc = cmd_verify_log(&o);
        else {
            usage(stderr);
            rc = 2;
        }
    }
    opts_free(&o);
    return rc;
}
