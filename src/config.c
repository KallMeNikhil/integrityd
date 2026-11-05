#include "integrityd.h"

#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char *const default_excludes[] = { "*.swp", "*.swx", "*~", ".#*" };

void config_init(Config *c)
{
    memset(c, 0, sizeof *c);
    vec_init(&c->watch);
    vec_init(&c->exclude);
    for (size_t i = 0; i < sizeof default_excludes / sizeof *default_excludes; i++)
        vec_push(&c->exclude, xstrdup(default_excludes[i]));
    c->mode = MODE_HYBRID;
    c->scan_interval = 60;
    c->deep_every = 10;
    c->startup_check = STARTUP_DEEP;
    c->batch_ms = 200;
    c->batch_max_ms = 1000;
    c->snapshot_flush_secs = 5;
    c->log_fsync = 1;
    c->log_max_bytes = 10485760;
    c->log_keep = 5;
    c->hash_max_bytes = 0;
    c->one_filesystem = 1;
    c->max_depth = 64;
    c->deep_verify_in_worker = 1;
    c->worker_min_records = 2000;
    c->alert_hook = NULL;
    c->hook_min_severity = SEV_ALERT;
    c->hook_rate_per_sec = 10;
}

void config_free(Config *c)
{
    vec_free_all(&c->watch, free);
    vec_free_all(&c->exclude, free);
    free(c->alert_hook);
    c->alert_hook = NULL;
}

int parse_mode(const char *s)
{
    if (strcmp(s, "scan") == 0) return MODE_SCAN;
    if (strcmp(s, "watch") == 0) return MODE_WATCH;
    if (strcmp(s, "hybrid") == 0) return MODE_HYBRID;
    return -1;
}

const char *mode_name(int mode)
{
    return mode == MODE_SCAN ? "scan" : mode == MODE_WATCH ? "watch" : "hybrid";
}

const char *sev_name(int sev)
{
    return sev == SEV_ALERT ? "ALERT" : sev == SEV_WARN ? "WARN" : "INFO";
}

int sev_from_name(const char *s)
{
    if (strcmp(s, "INFO") == 0) return SEV_INFO;
    if (strcmp(s, "WARN") == 0) return SEV_WARN;
    if (strcmp(s, "ALERT") == 0) return SEV_ALERT;
    return -1;
}

static int parse_num(const char *val, int64_t min, int64_t max, int64_t *out)
{
    char *end;
    errno = 0;
    long long v = strtoll(val, &end, 10);
    if (errno || end == val || *end || v < min || v > max) return -1;
    *out = v;
    return 0;
}

static const struct {
    const char *key;
    size_t      off;
    int         is64;
    int64_t     min, max;
} num_keys[] = {
    { "scan_interval", offsetof(Config, scan_interval), 0, 1, 86400 },
    { "deep_every", offsetof(Config, deep_every), 0, 1, 1000000 },
    { "batch_ms", offsetof(Config, batch_ms), 0, 1, 60000 },
    { "batch_max_ms", offsetof(Config, batch_max_ms), 0, 1, 600000 },
    { "snapshot_flush_secs", offsetof(Config, snapshot_flush_secs), 0, 0, 86400 },
    { "log_fsync", offsetof(Config, log_fsync), 0, 0, 1 },
    { "log_max_bytes", offsetof(Config, log_max_bytes), 1, 0, INT64_MAX },
    { "log_keep", offsetof(Config, log_keep), 0, 1, 100 },
    { "hash_max_bytes", offsetof(Config, hash_max_bytes), 1, 0, INT64_MAX },
    { "one_filesystem", offsetof(Config, one_filesystem), 0, 0, 1 },
    { "max_depth", offsetof(Config, max_depth), 0, 1, 4096 },
    { "deep_verify_in_worker", offsetof(Config, deep_verify_in_worker), 0, 0, 1 },
    { "worker_min_records", offsetof(Config, worker_min_records), 1, 0, INT64_MAX },
    { "hook_rate_per_sec", offsetof(Config, hook_rate_per_sec), 0, 1, 100000 },
};

int config_set(Config *c, const char *key, const char *val, Str *err)
{
    for (size_t i = 0; i < sizeof num_keys / sizeof *num_keys; i++) {
        if (strcmp(key, num_keys[i].key) != 0) continue;
        int64_t v;
        if (parse_num(val, num_keys[i].min, num_keys[i].max, &v) < 0) {
            str_appendf(err, "%s: invalid value '%s'", key, val);
            return -1;
        }
        char *field = (char *)c + num_keys[i].off;
        if (num_keys[i].is64)
            memcpy(field, &v, sizeof v);
        else {
            int iv = (int)v;
            memcpy(field, &iv, sizeof iv);
        }
        return 0;
    }
    if (strcmp(key, "watch") == 0) {
        char *real = realpath(val, NULL);
        if (!real) {
            str_appendf(err, "watch: %s: %s", val, strerror(errno));
            return -1;
        }
        vec_push(&c->watch, real);
    } else if (strcmp(key, "exclude") == 0) {
        if (!*val) {
            str_append(err, "exclude: empty pattern");
            return -1;
        }
        vec_push(&c->exclude, xstrdup(val));
    } else if (strcmp(key, "mode") == 0) {
        int m = parse_mode(val);
        if (m < 0) {
            str_appendf(err, "mode: invalid value '%s'", val);
            return -1;
        }
        c->mode = m;
    } else if (strcmp(key, "startup_check") == 0) {
        if (strcmp(val, "deep") == 0) c->startup_check = STARTUP_DEEP;
        else if (strcmp(val, "meta") == 0) c->startup_check = STARTUP_META;
        else if (strcmp(val, "none") == 0) c->startup_check = STARTUP_NONE;
        else {
            str_appendf(err, "startup_check: invalid value '%s'", val);
            return -1;
        }
    } else if (strcmp(key, "alert_hook") == 0) {
        free(c->alert_hook);
        c->alert_hook = NULL;
        if (*val) {
            if (val[0] != '/') {
                str_appendf(err, "alert_hook: must be an absolute path");
                return -1;
            }
            c->alert_hook = xstrdup(val);
        }
    } else if (strcmp(key, "hook_min_severity") == 0) {
        int s = sev_from_name(val);
        if (s != SEV_WARN && s != SEV_ALERT) {
            str_appendf(err, "hook_min_severity: must be WARN or ALERT");
            return -1;
        }
        c->hook_min_severity = s;
    } else {
        str_appendf(err, "unknown key '%s'", key);
        return -1;
    }
    return 0;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
    return s;
}

int config_load(Config *c, const char *path, Str *err)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        str_appendf(err, "%s: %s", path, strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t cap = 0;
    int lineno = 0, rc = 0;
    while (getline(&line, &cap, f) >= 0) {
        lineno++;
        char *s = trim(line);
        if (*s == '\0' || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            str_appendf(err, "%s:%d: expected key=value\n", path, lineno);
            rc = -1;
            continue;
        }
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);
        Str e;
        str_init(&e);
        if (config_set(c, key, val, &e) < 0) {
            str_appendf(err, "%s:%d: %s\n", path, lineno, e.p);
            rc = -1;
        }
        str_free(&e);
    }
    free(line);
    fclose(f);
    if (c->batch_max_ms < c->batch_ms) c->batch_max_ms = c->batch_ms;
    return rc;
}

int config_excluded(const Config *c, const char *relpath, int is_dir)
{
    const char *base = path_base(relpath);
    for (size_t i = 0; i < c->exclude.n; i++) {
        const char *pat = c->exclude.items[i];
        size_t n = strlen(pat);
        int dir_only = n > 1 && pat[n - 1] == '/';
        if (dir_only && !is_dir) continue;
        char *p = xstrndup(pat, dir_only ? n - 1 : n);
        int hit = fnmatch(p, base, 0) == 0 || fnmatch(p, relpath, 0) == 0;
        free(p);
        if (hit) return 1;
    }
    return 0;
}
