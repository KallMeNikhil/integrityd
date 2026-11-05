# integrityd — Build Blueprint

This is the implementation plan derived from `docs/DESIGN.md` (the spec). Where the
spec is silent or self-contradictory the resolution is recorded in `docs/DECISIONS.md`
(referenced here as `X-nn`).

## (a) Architecture in my own words

### Process model
One single-threaded process. Everything that can wake it up is a file descriptor
in one `poll()` call:

| fd | producer | what the loop does |
|---|---|---|
| self-pipe read end | signal handlers (`signals.c`) write one byte = signal number | dispatch SIGTERM/INT/HUP/USR1/USR2/ALRM/CHLD |
| `control.fifo` (read side, plus a held write side) | `integrityctl` clients | accumulate bytes, split lines, run a command, answer on the client's reply FIFO |
| inotify fd (modes `watch`/`hybrid`) | kernel | turn events into *paths* in a pending set, add/remove watches for directories |
| worker pipe (only while a deep-verify worker runs) | forked child | parse `path\tsha\tsize\tmtime\tctime` lines, re-`lstat`, classify |

The `poll` timeout is the earliest of three deadlines: the debounce batch deadline,
the snapshot flush deadline and the hook rate-limit window. A second process exists
only transiently: at most one deep-verify worker (`fork` + `pipe2`) and fire-and-forget
hook children (`fork` + `execve`), all reaped from SIGCHLD with `waitpid(WNOHANG)`.

### Data flow
```
           lstat/readdir                 hash_file (open/read/fstat)
scanner.c ──────────────► Vec<Record> ──────────────► compare.c (Engine)
inotify  ──► pending set ──► engine_reconcile ─────────┘     │ classify → Change list
worker   ──► (path, sha) ──► engine_apply_worker ──────┘     │ pair MOVED
                                                             ▼
                                   Snapshot (HashTab path→Record) advanced (D-16)
                                   emit() → logger.c (seq, hash chain, fsync, rotate)
                                          → hook.c (fork/execve, rate-limited)
snapshot.c saves the HashTab atomically (tmp + fsync + rename + dir fsync).
```
All detection paths (startup reconciliation, periodic scan, SIGUSR1, RESCAN, RESUME,
inotify batches, worker results, `integrityd check`) go through the **same** engine
(`compare.c`), so classification rules exist exactly once.

The engine is parameterised by an `emit` callback: the daemon emits into the
`change.log` logger (and the hook); `integrityd check` emits into a stdout logger.

### Event loop
Exactly the loop in spec 3.5. Signal handlers do nothing but
`write(selfpipe_w, &byte, 1)` with `errno` saved/restored. Debounce: first inotify
event sets `deadline = now + batch_ms`, each further event pushes it to
`min(now + batch_ms, first + batch_max_ms)`.

## (b) Module dependency order (bottom → top)

```
util.c (Str, Vec, xmalloc, time)          ← no deps
hashtab.c                                 ← util
sha256.c                                  ← none
escape.c                                  ← util
config.c                                  ← util
hashing.c                                 ← sha256
scanner.c                                 ← util, config, sha256 (symlink target hash)
snapshot.c                                ← util, hashtab, escape, sha256
logger.c                                  ← util, escape, sha256
compare.c (classify + Engine)             ← scanner, hashing, snapshot, escape
signals.c, daemonize.c                    ← util
control.c                                 ← util, escape
watcher.c                                 ← hashtab, scanner, config
worker.c                                  ← hashing, snapshot, escape
hook.c                                    ← util
daemon.c                                  ← everything above
main.c                                    ← daemon + offline commands
```
All modules except `main.c` are archived into `build/libintegrityd.a`; each unit test
links against it.

## (c) Milestones

| S | Files created | Unit tests | Integration tests |
|---|---|---|---|
| S0 | `Makefile`, `src/integrityd.h`, `src/util.c`, `src/hashtab.c`, `src/main.c` (version), `tests/unit/check.h`, `tests/unit/test_util.c`, `tests/unit/test_hashtab.c`, `tests/run_integration.sh` | Str/Vec/HashTab (incl. growth, delete, iteration) | — |
| S1 | `src/config.c`, `src/scanner.c`, `check --scan-only` in `main.c` | `test_config`, `test_scanner` (fixture tree, excludes, sort) | T02 (symlink loop, dangling link, FIFO: no hang) |
| S2 | `src/sha256.c`, `src/hashing.c`, `hash` subcommand | `test_sha256` (NIST vectors, lengths 55/56/63/64/65, streaming split), `test_hashing` (unstable-file retry via forked writer, unreadable, FIFO not opened) | T04 (vs `sha256sum`, 0..100 MB) |
| S3 | `src/escape.c`, `src/snapshot.c`, `baseline` subcommand | `test_escape` (random-byte round trip), `test_snapshot` (round trip with weird names, corruption detection, atomic replace) | T01 (partially: baseline), T03 (snapshot part) |
| S4 | `src/compare.c`, `src/logger.c`, `check`, `verify-log` | `test_compare` (table: every classify row + MOVED pairing), `test_logger` (chain, continue across reopen, tamper detection, rotation) | T01, T05–T10, T12 (offline via `check`), T19, T20 |
| S5 | `src/watcher.c`, `src/signals.c` (self-pipe), `src/daemon.c` loop, `daemon --foreground` | — | T05–T13 against the live daemon (≈1 s latency) |
| S6 | `src/control.c`, `scripts/integrityctl`, SIGUSR1/HUP/ALRM/CHLD handling | `test_control` (request parsing) | T03 (control part), T14, T15, T16, T17 |
| S7 | `scripts/report.sh`, `scripts/launch.sh` | — | T24 |
| S8 | `src/daemonize.c`, pid file, startup reconciliation, rotation wired into daemon | — | T18, T20 (daemon), T21, launch.sh start/stop |
| S9 | `src/worker.c`, `src/hook.c` | — | T22, T23 |
| S10 | `README.md`, `scripts/demo.sh`, `make asan`, DECISIONS final | — | whole suite under ASan/UBSan, demo end to end |

`make test` = build with `-Werror`, run every unit test binary, then every
`tests/integration/T*.sh`. `make asan` = rebuild into `build-asan/` with
`-fsanitize=address,undefined -fno-omit-frame-pointer -g` and run the same suite
against that binary (`INTEGRITYD=build-asan/integrityd`).

## (d) Contradictions, ambiguities and risks found

1. **`stealth=1` can never fire as written.** 5.4(4a) defines stealth as
   `(dev,ino,size,mtime_ns,ctime_ns)` all equal, but 4.1 itself states user space
   cannot restore ctime, so a `touch -r` stealth edit (T07) always changes ctime.
   Resolution: stealth = `(dev,ino,size,mtime_ns)` equal — i.e. everything `ls -l`
   shows looks untouched. → X-01.
2. **Rule 4b vs chmod on an unhashable file.** A pure `chmod` changes ctime, so
   rule 4b would also emit `MODIFIED basis=meta`. Resolution: when PERMS fires,
   rule 4b compares `(size, mtime_ns)` only. → X-02.
3. **"Nothing advances for UNREADABLE"** would hide `chmod 000` (a real PERMS
   change) forever. Resolution: metadata advances and PERMS is reported; only the
   content hash is frozen until the file is readable again. → X-03.
4. **`check` exit codes**: 7.1 says `1` = changes *or* general error, section 6
   says `2` = error. Section 6 is more specific → 0 clean / 1 changes / 2 error. → X-04.
5. **S1 asks for `check --scan-only`**, which section 6 doesn't list. Added. → X-04.
6. **`check --update` advances the snapshot**, but the spec says only that events go
   to stdout. Advancing the baseline without an audit record would break D-16
   ("each change reported once" in the log), so `--update` also appends the events to
   `change.log`. `baseline` appends a `REBASELINE` record. → X-05.
7. **Run dir inside a watched tree** would make every log write an event (feedback
   loop). The run dir is always excluded. → X-06.
8. **Reply FIFO path comes from the client**; opening an arbitrary path `O_WRONLY`
   could write into a regular file. It must be inside `<rundir>/reply/`, is opened
   `O_NOFOLLOW`, and must be a FIFO. → X-07.
9. **Partial last log line** (T19): append mode would glue new records onto the
   fragment. The daemon truncates to the last complete line and logs an `ERROR`;
   `verify-log` reports `TRUNCATED` (exit 4). → X-08.
10. **`log_keep` deletes the oldest rotated file**, so the chain's first link can
    disappear. `verify-log` then anchors on the first available record and says so. → X-09.
11. **T22 needs a worker run**, but the worker only runs for `records > 2000`.
    The threshold is a config key `worker_min_records` (default 2000). → X-10.
12. **Untestable without root**: inotify queue overflow and watch-limit `ENOSPC`
    need `sysctl` changes. Implemented, but covered only by code review and a
    manual procedure in the README. Risk accepted. → X-11.
13. **Development host is Windows + WSL2.** The repo lives on a 9p (`drvfs`) mount
    where every file is mode 0777 and FIFOs can't be created. All tests therefore run
    in `mktemp -d` directories on the Linux filesystem, and git uses
    `core.fileMode=false` with executable bits set in the index. → X-12.
14. **Timing in integration tests**: real inotify + debounce means tests must wait.
    Helpers poll the log for an expected line with a timeout (no fixed sleeps longer
    than needed) to stay fast and not flaky.
