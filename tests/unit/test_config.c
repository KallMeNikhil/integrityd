#include "integrityd.h"
#include "check.h"

#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    Config c;
    config_init(&c);
    CHECK(c.mode == MODE_HYBRID && c.scan_interval == 60 && c.deep_every == 10);
    CHECK(c.startup_check == STARTUP_DEEP && c.batch_ms == 200 && c.batch_max_ms == 1000);
    CHECK(c.log_max_bytes == 10485760 && c.log_keep == 5 && c.hook_min_severity == SEV_ALERT);
    CHECK(c.exclude.n == 4);

    CHECK(config_excluded(&c, "a/b.swp", 0));
    CHECK(config_excluded(&c, "notes.txt~", 0));
    CHECK(config_excluded(&c, "x/.#lock", 0));
    CHECK(!config_excluded(&c, "a/b.txt", 0));

    char dir[] = "/tmp/itest_cfg.XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char *path = path_join(dir, "integrityd.conf");
    FILE *f = fopen(path, "w");
    fprintf(f, "# comment\n\n  mode = scan  \nscan_interval=5\nexclude=cache/\nexclude=*.log\n"
               "watch=%s\nalert_hook=/bin/true\nhook_min_severity=WARN\nstartup_check=meta\n"
               "log_max_bytes=1234\n",
            dir);
    fclose(f);
    Str err;
    str_init(&err);
    CHECK(config_load(&c, path, &err) == 0);
    CHECK(c.mode == MODE_SCAN && c.scan_interval == 5 && c.startup_check == STARTUP_META);
    CHECK(c.log_max_bytes == 1234 && c.hook_min_severity == SEV_WARN);
    CHECK_STREQ(c.alert_hook, "/bin/true");
    CHECK(c.watch.n == 1);
    CHECK(config_excluded(&c, "cache", 1));
    CHECK(!config_excluded(&c, "cache", 0));
    CHECK(config_excluded(&c, "deep/x/app.log", 0));

    f = fopen(path, "w");
    fprintf(f, "mode=fast\nnonsense\nbogus=1\nscan_interval=-3\nwatch=/does/not/exist\n");
    fclose(f);
    str_clear(&err);
    CHECK(config_load(&c, path, &err) < 0);
    CHECK(strstr(err.p, ":1: mode") != NULL);
    CHECK(strstr(err.p, ":2: expected key=value") != NULL);
    CHECK(strstr(err.p, ":3: unknown key") != NULL);
    CHECK(strstr(err.p, ":4: scan_interval") != NULL);
    CHECK(strstr(err.p, ":5: watch") != NULL);

    str_clear(&err);
    CHECK(config_set(&c, "alert_hook", "relative/hook", &err) < 0);
    CHECK(config_set(&c, "hook_min_severity", "INFO", &err) < 0);
    CHECK(parse_mode("watch") == MODE_WATCH && parse_mode("x") < 0);
    CHECK(sev_from_name("ALERT") == SEV_ALERT);

    str_free(&err);
    unlink(path);
    rmdir(dir);
    free(path);
    config_free(&c);
    return CHECK_DONE();
}
