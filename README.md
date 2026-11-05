# integrityd: a Unix file-integrity daemon

integrityd watches one or more directory trees and reports exactly what changed, as soon
as it changes: creations, content edits (including "stealth" edits that restore size and
mtime), deletions, permission and ownership changes, renames, and type changes. It keeps
an inode-level baseline (`snapshot.db`), writes a tamper-evident, hash-chained audit log
(`change.log`), can be steered live through a control FIFO, and turns the log into a
plain-language report with a shell script.

It is written in C11 (libc + POSIX/Linux headers only, SHA-256 included) and Bash.
Design: [`docs/DESIGN.md`](docs/DESIGN.md) (the spec), [`docs/BLUEPRINT.md`](docs/BLUEPRINT.md)
(architecture and plan), and [`docs/DECISIONS.md`](docs/DECISIONS.md) (choices made where the spec was silent).

**New here?** [`docs/GUIDE.md`](docs/GUIDE.md) walks through installation on a fresh system,
testing, and a complete feature-by-feature demo for evaluation.

> **Linux only.** Real-time detection uses inotify, so you need a real Linux kernel: an
> Ubuntu VM, bare metal, or WSL2. It does not work on WSL1. `--mode scan` uses only
> `alarm()` + `opendir/readdir/lstat` and works everywhere.

## Build and test

```bash
sudo apt-get install -y build-essential gawk procps
make
make test
make asan
```

`make` produces `build/integrityd`, `make test` runs the unit and integration tests
(T01–T25), and `make asan` runs the same suite under AddressSanitizer and UBSan.
Every file compiles with `gcc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wpedantic -Werror`.
Integration tests create their trees and run dirs with `mktemp -d` and run real daemons
against the real kernel. One test can be run alone with `make integration T=T07`.

## Run

```bash
./scripts/launch.sh start --watch ~/demo
./scripts/launch.sh status
tail -f ~/.integrityd/change.log
./scripts/report.sh
./scripts/launch.sh stop
```

The run directory defaults to `~/.integrityd/` (or `/var/lib/integrityd` for root). Every
tool takes `--rundir DIR`; the shell scripts also read the `INTEGRITYD_RUNDIR` environment
variable (the `integrityd` binary does not). It contains:

| File | What |
|---|---|
| `snapshot.db` | baseline: one record per path, sorted, with a checksum trailer (atomic replace) |
| `change.log`, `change.log.N` | append-only, hash-chained event log (rotated at `log_max_bytes`) |
| `control.fifo`, `reply/` | control protocol (FIFO 0600, run dir 0700) |
| `integrityd.pid`, `daemon.out` | pid file; stdout/stderr of the detached daemon |
| `integrityd.conf` | optional config (`key=value`) |

### Command line

```
integrityd daemon   [--rundir D] [--config F] [--watch DIR ...] [--mode hybrid|scan|watch] [--foreground]
integrityd baseline --watch DIR ... [--rundir D]
integrityd check    [--rundir D] [--deep] [--update]
integrityd check    --scan-only [--watch DIR]
integrityd hash     FILE...
integrityd verify-log [--rundir D | --log FILE]
integrityd version
```

| Command | Purpose |
|---|---|
| `baseline` | create or replace the snapshot, without alerts |
| `check` | offline compare; exit 0 clean, 1 changes, 2 error, 4 corrupt snapshot |
| `check --scan-only` | print the scanner's records |
| `hash` | same output format as `sha256sum` |
| `verify-log` | prints `OK <n> head=<chain>`, or `BROKEN <seq>` with exit 4 |

Roots belong to the snapshot. `--watch` and `watch=` only add roots that aren't known yet.
After the first start, `launch.sh start` needs no `--watch`.

### Control commands (`scripts/integrityctl`)

| Command | Effect |
|---|---|
| `ping`, `version` | liveness, version string |
| `status` | `state= mode= roots= records= watches= uptime_s= last_scan= last_scan_ms= alerts_total= log_seq= pid= worker=` |
| `pause` / `resume` | stop alerting (events are still collected); resume reports everything that changed, tagged `during_pause=1` |
| `rescan [PATH]` | immediate metadata scan of everything or one subtree |
| `deepverify` | hash every file (in a forked worker for big trees) |
| `add PATH` / `remove PATH` | add a root (silent baseline, live watches) / drop a root |
| `list` | roots, one per line |
| `rebaseline [PATH]` | adopt the current state as the baseline, without alerts |
| `reload` | re-read the config (same as `kill -HUP`) |
| `tail [N]` | last N (max 200) log lines |
| `shutdown` | same as `kill -TERM` |
| `kill-rescan` | sends `SIGUSR1`: the on-demand full check |

Exit status: 0 on `OK`, 2 on `ERR <code> <msg>`, 1 if the daemon doesn't answer.

### Signals

| Signal | Effect |
|---|---|
| `SIGTERM`, `SIGINT` | finish the current batch, save the snapshot, log `DAEMON_STOP`, exit 0 |
| `SIGHUP` | reload the config (not the root set) |
| `SIGUSR1` | immediate full rescan: `kill -USR1 $(cat ~/.integrityd/integrityd.pid)` |
| `SIGUSR2` | write a `STATUS` line to the log |
| `SIGALRM` | the periodic scan timer (`scan_interval`); every `deep_every`-th scan hashes everything |

The handlers only write one byte to a self-pipe. All work happens in the `poll()` loop.

### Reports

```bash
./scripts/report.sh
./scripts/report.sh --since "2026-10-31 09:00"
./scripts/report.sh --format text
./scripts/report.sh --format html
./scripts/report.sh --events
```

The default is a one-line summary. `--since` and `--until` use UTC, as in the log. `text` adds
counts per event, recent alerts in plain words and the top paths. `html` writes a static
`<rundir>/report.html` with no JavaScript. `--events` includes INFO events.

## Configuration (`integrityd.conf`)

Edit it in vi, then `integrityctl reload`. Lines are `key=value`; lines starting with `#` are comments.

| Key | Default | Meaning |
|---|---|---|
| `watch` (repeatable) | — | seed roots (only used for roots not yet in the snapshot) |
| `exclude` (repeatable) | `*.swp *.swx *~ .#*` | fnmatch patterns on basename and root-relative path; trailing `/` = directories only; added to the defaults |
| `mode` | `hybrid` | `scan` (timer only), `watch` (inotify only), `hybrid` (both) |
| `scan_interval` | 60 | seconds between periodic scans |
| `deep_every` | 10 | every Nth periodic scan hashes all files |
| `startup_check` | `deep` | how offline changes are found at start: `deep`, `meta`, `none` |
| `batch_ms` / `batch_max_ms` | 200 / 1000 | inotify debounce window / hard cap |
| `snapshot_flush_secs` | 5 | max delay before the snapshot is saved after event batches |
| `log_fsync` | 1 | fsync every log record |
| `log_max_bytes` / `log_keep` | 10485760 / 5 | rotation |
| `hash_max_bytes` | 0 | larger files are compared by size/mtime/ctime only (0 = no limit) |
| `one_filesystem` | 1 | don't cross mount points |
| `max_depth` | 64 | directory depth limit |
| `deep_verify_in_worker` / `worker_min_records` | 1 / 2000 | run deep passes in a forked worker when there are more records than this |
| `alert_hook` | — | absolute path of an executable run as `hook EVENT PATH SEVERITY SEQ` with `INTEGRITYD_DETAIL` in its environment; must not be group/world-writable |
| `hook_min_severity` | `ALERT` | `WARN` or `ALERT` |
| `hook_rate_per_sec` | 10 | burst limit; dropped calls are reported as `hook_dropped=N` |

## Events

| Event | Severity | When |
|---|---|---|
| `CREATED` | WARN | new path |
| `MODIFIED` | ALERT | content changed; `stealth=1` if size and mtime look untouched; `basis=meta` if the file couldn't be hashed |
| `DELETED` | ALERT | path gone |
| `PERMS` | ALERT | mode/uid/gid changed (separate from content changes) |
| `TYPE_CHANGED` | ALERT | file ↔ directory ↔ symlink |
| `MOVED` | WARN | same inode and same content at a new path (`from=`) |
| `REPLACED` | INFO | same content, new inode (editor atomic save) |
| `UNREADABLE`, `UNSTABLE` | WARN | permission denied (logged once) / file kept changing while hashed |
| `ERROR` | WARN | I/O or internal problems (`what=`) |
| `DAEMON_START`, `DAEMON_STOP`, `SCAN_END`, `PAUSED`, `RESUMED`, `REBASELINE`, `WATCH_ADDED`, `WATCH_REMOVED`, `CONFIG_RELOAD`, `OVERFLOW_RESCAN`, `LOG_ROTATE`, `STATUS` | INFO | system events |

Log line: `seq TAB timestamp TAB severity TAB event TAB path TAB k=v;k=v TAB chain`, where
`chain = SHA256(previous_chain "\n" line-without-chain)`. Paths are percent-escaped, so
spaces, tabs, newlines and non-UTF-8 bytes in file names are safe.

## Demo (spec section 12)

`scripts/demo.sh` runs the whole demo unattended in a temp dir (`--pause` waits between
steps). By hand:

1. `./scripts/launch.sh start --watch ~/demo`, then show `ps -o pid,ppid,sid,tty,comm -p $(cat ~/.integrityd/integrityd.pid)`, the pid file and `daemon.out`.
2. In another terminal: `echo hi >> a.txt; touch new; rm old; chmod 600 secret`. Show `tail -f ~/.integrityd/change.log` and `./scripts/report.sh`.
3. Stealth edit: `touch -r f ref; printf 'X…' > f; touch -r ref f`. `ls -l` looks the same, but the log shows `MODIFIED … stealth=1`.
4. `integrityctl pause` / `resume`, `integrityctl add ~/other`, `kill -USR1 $(cat ~/.integrityd/integrityd.pid)`.
5. `launch.sh stop`, edit files, `launch.sh start`: the changes are reported with `offline=1`.
6. Edit one log line, then run `integrityd verify-log`: it prints `BROKEN <seq>` and exits 4.

Fallback if inotify misbehaves in the VM: `--mode scan` with `scan_interval=2` in the config.

## Manual checks not in the automated suite

These need root to provoke (see DECISIONS X-11):

- **inotify queue overflow:** `sudo sysctl fs.inotify.max_queued_events=16`, start the daemon,
  freeze it with `kill -STOP <pid>`, run `for i in $(seq 500); do touch ~/demo/f$i; done`, then
  `kill -CONT <pid>`. Expect `OVERFLOW_RESCAN`, followed by a scan that reports every file.
  Restore the old value afterwards.
- **watch limit:** `sudo sysctl fs.inotify.max_user_watches=10` with a tree of more than 10 directories.
  Expect `ERROR what=watch_limit;fallback=scan`; that root is then covered by periodic scans.

## Limitations

Linux only. Someone with root can change the daemon, the snapshot and the log together
(the hash chain only proves the log is internally consistent: copy `head=` from
`verify-log` somewhere else to anchor it). ctime can be forged by root changing the clock.
Extended attributes/ACLs aren't tracked. Renames aren't tracked across roots.
