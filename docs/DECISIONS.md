# Implementation decisions (spec gaps and resolutions)

The spec (`docs/DESIGN.md`) has the Decision Register D-01…D-26. This file records the
choices made *while implementing*, where the spec is silent or contradicts itself.
Rule used throughout: the simplest option consistent with spec section 0.2.

| ID | Topic | Decision | Why |
|---|---|---|---|
| X-01 | `stealth=1` | Set when `(dev, ino, size, mtime_ns)` are unchanged but the hash differs. ctime is **not** part of the test. | Spec 4.1 says ctime can't be restored from user space, so the literal 5.4(4a) rule could never fire and T07 would be unsatisfiable. |
| X-02 | Rule 4b (`basis=meta`) | Compares `(size, mtime_ns, ctime_ns)`; if mode/uid/gid also changed (PERMS fires), ctime is ignored. | Otherwise every chmod of an unhashable file also reports a fake content change. |
| X-03 | UNREADABLE baseline policy | Metadata (mode, uid, …) advances and PERMS is reported; the stored content hash is kept until the file is readable again. A *new* unreadable file is recorded with sha `-` and reported `CREATED` + `UNREADABLE`. | "Nothing advances" would hide a `chmod 000` forever; freezing only the hash keeps 5.4(8)'s intent (no content baseline from an unread file). |
| X-04 | `check` CLI | Exit 0 clean / 1 changes / 2 error (section 6). Flags: `--deep`, `--update`, `--scan-only` (S1: print records in snapshot format, no compare). | 7.1 overloads exit 1; section 6 is specific. S1 requires `--scan-only`. |
| X-05 | Offline commands and the log | `baseline` appends `REBASELINE` to `change.log`; `check --update` appends its events to `change.log` before saving the snapshot. Plain `check` touches nothing. Both refuse (exit 3) while a daemon owns the run dir. | Advancing the baseline without an audit record would break D-16. |
| X-06 | Excludes | `exclude=` lines are **added** to the default patterns. The run dir (and anything under it) is always excluded. | A run dir inside a watched tree would otherwise produce an endless event feedback loop. |
| X-07 | Reply FIFO safety | Reply path must lie inside `<rundir>/reply/`; opened `O_WRONLY\|O_NONBLOCK\|O_NOFOLLOW`; must be a FIFO (`fstat`). On `EAGAIN` the server waits for `POLLOUT` at most 1 s in total, then drops the rest. | Never write into an arbitrary file; never block the loop for long (big `TAIL` replies exceed the 64 KiB pipe buffer). |
| X-08 | Partial last log line | On open the daemon truncates `change.log` back to its last complete line and logs `ERROR what=log_partial_line;bytes=N`. `verify-log` reports `TRUNCATED <seq>` and exits 4. | Appending after a fragment would corrupt the next record; a tamper-evidence tool must flag it. |
| X-09 | Chain anchor after rotation | If the oldest file's first record has `seq > 1`, verification trusts its `chain` as the anchor and prints `anchor=seq N`. | `log_keep` deletes the beginning of the chain by design. |
| X-10 | Worker threshold | Config key `worker_min_records` (default 2000) replaces the hard-coded "records > 2000". | Makes T22 testable with a small tree. |
| X-11 | Overflow / watch limit tests | Implemented per 5.5 but not in the automated suite (needs root to shrink `max_queued_events` / `max_user_watches`). Manual procedure in README. | Tests must run as a normal user. |
| X-12 | Dev environment | Built on WSL2 Ubuntu 26.04 (gcc 15). Repo is on a 9p mount (all files 0777, no FIFOs), so tests use `mktemp -d` on the Linux filesystem; git uses `core.fileMode=false`, scripts are `+x` in the index. | Host constraint. |
| X-13 | Pid file | Also written in `--foreground` mode (tests use it for `kill -USR1`). Created with `O_EXCL` after the liveness check, so two simultaneous starts can't both win. | Simplest race-free variant. |
| X-14 | No roots | The daemon may start with zero roots (warning on stderr); roots can be added with `ADD`. | Spec doesn't forbid it; useful for control-pipe demos. |
| X-15 | `watch` mode timer | `alarm()` stays armed in `watch` mode but a tick only retries watches on missing/degraded roots (no scan). | 7.2 requires missing roots to be retried "on each periodic scan". |
| X-16 | File roots | A root that is a regular file is watched with a non-directory mask (no `IN_ONLYDIR`). If its watch disappears (atomic save), the path is reconciled and the watch re-added. | Spec allows `ADD <file>`. |
| X-17 | Directory renames | MOVED pairing stays regular-files-only (5.4(7)); a moved directory gives MOVED for its files and DELETED+CREATED for the directory records. On `IN_MOVED_FROM\|IN_ISDIR` the watches of that subtree are removed; `IN_MOVED_TO` re-adds them. | Watches follow inodes; stale wd→path maps would mislabel later events. |
| X-18 | RESUME order | Log `RESUMED`, reconcile the pending set (hashing), then run a full metadata scan; all events tagged `during_pause=1`, then `SCAN_END`. | "Pause never creates a blind spot." |
| X-19 | `check` output | Log-format lines on stdout with their own `seq` (from 1) and a fresh chain. | Same parser (`report.sh`, `awk`) works on both. |
| X-20 | Unreadable directories | While a directory is unreadable, its baseline descendants are not reported DELETED. | Can't distinguish "gone" from "can't look". |
| X-21 | Hook problems | Invalid hook → `ERROR what=hook_rejected;why=…`, hook disabled, daemon keeps running. Rate-limit drops → `ERROR what=hook_rate;hook_dropped=N` once per second. Events from the hook module itself never fire the hook. | Avoid recursion; never die because of an optional feature. |
| X-22 | Severity order | `INFO < WARN < ALERT`. | Needed for `hook_min_severity`. |
| X-23 | `integrityctl` options | `integrityctl [--rundir D] <command> [args]`; run dir also from `$INTEGRITYD_RUNDIR`. Same for `launch.sh` and `report.sh` (`--rundir`). | Tests use private run dirs. |
| X-24 | `SCAN_END` detail | `kind=meta\|deep\|resume;files=N;changes=N;ms=N`. | Spec lists files/changes/ms; kind helps the report. |
| X-25 | Initial baseline | Silent: no CREATED/UNREADABLE events, only `DAEMON_START baseline=initial;records=N`. | Spec 3.4: "no alerts". |
| X-26 | Deep pass in worker | Deep pass with a worker = inline metadata scan (create/delete/perms) + worker hashes every regular file. `SCAN_END kind=deep` is logged when the worker finishes. | Worker protocol carries only content facts. |
| X-27 | `#CREATED` | Time this snapshot file was written. | Simplest. |
| X-28 | Extra test | `T25_edge_cases.sh` covers the 7.2 rows without a T-number (root deleted/re-created, unreadable file and dir, FIFO created live, file↔dir, excludes, file root, `hash_max_bytes`). | 7.2 says every row has a test. |
| X-29 | Scans while paused | While paused, `RESCAN`/`DEEPVERIFY` answer `ERR 409`, SIGUSR1 and periodic ticks are ignored, and deep-worker results are discarded. RESUME performs the full scan. | Pause means "no alerts"; RESUME is the catch-up point. |
| X-30 | Run dir mode | The daemon `chmod`s an existing run dir to 0700 at start. | 7.3; found by T14 when the dir pre-existed with 0755. |
| X-31 | MOVED + chmod | A MOVED file whose mode/owner also changed gets an extra `PERMS` event. | A move must not hide a permission change. |
| X-32 | `LIST` output | Roots are percent-escaped like request arguments (space → `%20`), one `OK <root>` line each. | Newlines in root names can't break the line protocol. |
| X-33 | `integrityctl raw` | Hidden subcommand that sends its arguments as the command line; used by T14 to send malformed/unknown commands. | Testing aid. |
| X-34 | Shutdown with a running worker | The worker gets SIGTERM and is reaped; its partial results are discarded. | Spec only requires a graceful stop. |
