# integrityd — Final Build-Ready Specification
### Unix File Integrity Daemon

**Subject:** Unix Programming Lab (course 24B15CS214) — mini project (20 marks)
**Team:** solo · **Language:** **C (C11, GCC)** + Bash · **Platform:** Ubuntu VM (native Linux kernel with inotify) · **Hash:** SHA-256 (self-contained implementation)
**Deliverables:** the codebase now; the project report is written later, after the code is complete (Section 16).

> **Legend:** `[ORIGINAL]` = from the original handbook (`integrityd.html`) · `[REVISED]` = original idea kept, details changed · `[ADDED]` = new decision/spec filling an audit gap. `D-xx` = row in the Decision Register (Section 15). Decisions are **final**.

---

## 0. Project Rules

### 0.1 Authority
This file is the single source of truth. Where it is silent, the simplest option that satisfies 0.2 is used and recorded in `docs/DECISIONS.md`.

### 0.2 Hard constraints
1. **Language: C11, compiled with `gcc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wpedantic -Werror`; shell scripts in Bash. No Python.** Why C and not C++ (D-01): the syllabus is C-based (module 1 "writing C program in Unix using GCC", modules 5–6 stdio and system calls); the original handbook is C; every API used (`opendir`, `lstat`, `inotify`, `fork`, `sigaction`, `mkfifo`) is a C API, so the code maps one-to-one to what the examiners teach and ask in viva; and a daemon that forks and handles signals is simpler to reason about in C (no RAII/destructor/exception behaviour around `fork`, `_exit` and signal handlers). The few helpers C lacks (growable string/array, one hash table) are small and listed in 5.12.
2. **Libraries (D-01):** the C standard library, POSIX and Linux headers are all allowed (`qsort`, `bsearch`, `snprintf`, `getopt_long`, `fnmatch`, `poll`, `realpath`, …). **No third-party libraries** and no OpenSSL: SHA-256 is one self-contained file (`sha256.c`, FIPS 180-4, ~120 lines). Do not reinvent what libc already offers; write only the helpers in 5.12.
3. **Syllabus-centred design (D-02):** the *core* must be built from primitives named in the course syllabus — `opendir/readdir/closedir`, `stat/lstat`, inodes, standard I/O (`fopen/fprintf/fgets/fclose`), low-level I/O (`open/read/write/close/dup2`), `fork/exec/wait`, pipes and FIFOs, `signal`/`sigaction`, `kill`, `alarm`, shell scripting with `grep/sed/awk`, loops, functions and `trap`. Small, explicitly marked diversions are allowed: `inotify`, `poll`, `fnmatch`, `getopt_long`.
4. **Runs without root.** Default run directory `~/.integrityd/`. Everything must also work when started as root (then default `/var/lib/integrityd`).
5. **Linux only.** Document that inotify requires a real Linux kernel (Ubuntu VM or WSL2, not WSL1).
6. Front end: none. The "UI" is the command line, `integrityctl`, and `report.sh` (optionally a static HTML report, Section 8).

### 0.3 Working rules
- Build **milestone by milestone** (Section 9). Each milestone ends with `make test` green and `git commit -m "S<n>: <title>"`.
- All code paths that touch the kernel check return values; no fixed-size path buffers (use the `Str` helper from 5.12; never assume `PATH_MAX`).
- Signal handlers do **only** async-signal-safe work (write one byte to the self-pipe, nothing else).

### 0.4 Repo layout (D-03) `[REVISED]`
```
integrityd/
├── Makefile                      # all, test, asan, clean ; flags in 0.2
├── README.md
├── docs/ DESIGN.md (this file), DECISIONS.md
├── src/
│   ├── main.c                    # getopt_long + subcommand dispatch (daemon/baseline/check/hash/verify-log/version)
│   ├── integrityd.h              # shared types: Record, Event, Config; prototypes
│   ├── util.c                    # Str (growable string), Vec (growable array), die/xmalloc, time helpers
│   ├── hashtab.c                 # one chained hash table: string key → pointer (path→Record, wd→dir, dir→wd)
│   ├── config.c                  # key=value config parser + defaults + reload
│   ├── sha256.c                  # FIPS 180-4, streaming interface
│   ├── hashing.c                 # file hashing via open/read (low-level I/O), fstat stability check
│   ├── escape.c                  # percent-encoding for snapshot/log fields
│   ├── scanner.c                 # opendir/readdir/lstat recursive walk
│   ├── snapshot.c                # load/save (stdio), checksum trailer, atomic replace
│   ├── compare.c                 # classification of old vs new records, MOVED pairing
│   ├── logger.c                  # append-only change log, hash chain, fsync, rotation
│   ├── watcher.c                 # inotify: recursive watches, wd↔path maps, debounce, overflow
│   ├── signals.c                 # sigaction + self-pipe
│   ├── control.c                 # FIFO command protocol (server side)
│   ├── daemonize.c               # double fork, setsid, pid file, fd redirection
│   ├── worker.c                  # fork()'d deep-verify worker + pipe protocol
│   ├── hook.c                    # alert hook via fork/execve
│   └── daemon.c                  # event loop (poll) tying everything together
├── scripts/
│   ├── launch.sh                 # start|stop|restart|status
│   ├── integrityctl              # control client (FIFO request/reply)
│   └── report.sh                 # log → readable summary (text|oneline|html)
└── tests/
    ├── unit/                     # C unit tests (tiny CHECK macros)
    └── integration/              # bash scenario scripts (T01…T24, Section 11)
```
(Original layout: `scanner.c watcher.c hash.c control.c signals.c logger.c integrityd.h Makefile scripts/report.sh scripts/launch.sh data/{snapshot.db,change.log,control.fifo}` — all kept, `hash.c` is now `hashing.c`+`sha256.c`; `main.c` and the remaining modules were missing in the original layout and are added.)

---

## 1. Project Definition

### 1.1 What it does `[ORIGINAL]`
integrityd is a background daemon that watches a directory tree and reports exactly what changed — the moment it changes. Built entirely on POSIX/Linux system calls: no external libraries, no frameworks. It takes a snapshot of a directory — every file's size, timestamp, permissions, and content fingerprint — and keeps that snapshot up to date in real time. Any creation, edit, deletion, or permission change is caught, logged, and reported in plain language, without needing to check back manually.

Four course-concept areas it demonstrates:
- **Directory & inode handling (file system):** recursive traversal of the watched tree, reading each file's inode-level metadata directly.
- **Shell scripting & filters (automation):** a shell layer controls the daemon and turns raw logs into a readable summary using text-processing filters.
- **Standard I/O (persistence):** buffered I/O writes the snapshot store and the append-only change log.
- **Processes & signals (control):** signal handling and a control pipe let the running daemon be queried or redirected without restarting it.

### 1.2 Problem statement and differentiator `[ADDED]`
*Problem:* silent changes to important files (configs, scripts, binaries, web roots) are hard to notice; checking by hand does not scale, and a plain `ls -l` can be fooled by restoring timestamps.
*Existing tools:* AIDE/Tripwire (periodic, heavy, not educational), `inotifywait` (events only, no integrity baseline), `auditd` (system-wide, root).
*What integrityd adds (the report's differentiator, D-04):* a **small, fully readable implementation** that combines (1) an inode-level baseline, (2) real-time kernel events **and** a periodic safety-net scan, (3) tamper-evident, hash-chained logs, (4) live control without restart, and (5) a shell reporting layer — every piece mapped to a syllabus topic.

### 1.3 Threat model `[ADDED]` (D-05)
**In scope:** accidental or malicious modification of watched files by processes running as non-root users or by the same user; content edits with preserved size/mtime; permission/ownership changes; file replacement; deletions; log edits by a non-privileged attacker (detected by the hash chain); stale/corrupt snapshot.
**Out of scope (state in the report):** an attacker with root who can modify the daemon, its snapshot and its log together; kernel rootkits; raw block-device writes (partly mitigated by the periodic deep hash); remote/off-host log shipping. Recommended deployment note: run as root with a root-owned run directory for stronger guarantees; print the log chain head to a second location to anchor it.

### 1.4 Capabilities `[ORIGINAL]` (all retained)
- **Recursive Scan** — walks the full tree, including nested subfolders, on first run.
- **Real-Time Detection** — reacts the instant a change happens instead of checking on a fixed interval. `[REVISED]` realised by inotify, backed by a periodic safety-net scan (hybrid mode, D-06).
- **Content Fingerprinting** — hashes file contents (SHA-256) so silent edits are caught even if size and timestamp look unchanged.
- **Metadata Tracking** — size, modification time, permission bits (plus inode, owner, group, link count, ctime `[ADDED]`) for every watched file.
- **Persistent Log** — every alert is timestamped and appended to a durable log, forming an audit trail.
- **Live Control Pipe** — send commands to the running daemon (pause, resume, status, add a path, …) without restarting it.
- **On-Demand Rescan** — a single signal forces an immediate full check outside its normal rhythm.
- **Readable Reports** — a wrapper script turns the raw log into a one-line, human-readable summary on request.
- **Durable Snapshot Store** — baseline state is written to disk, so the daemon survives restarts without losing its reference point (and reports offline changes on restart, `[ADDED]`).

---

## 2. Syllabus Alignment `[ADDED]`

Source: *Unix Programming Lab* syllabus (24B15CS214; COs: CO1 Unix commands, CO2 run C/C++ programs on Unix, CO3 shell programming, CO4 Unix system calls). Evaluation: Mid-sem (P-1) 20 · Internal assessment 40 (attendance 15, class test/quiz/submission 5, **mini-project 20**) · written theory 40.

| Syllabus module | Where integrityd uses it |
|---|---|
| 1. Unix OS, open source, **GCC** | `Makefile` with `gcc`; compile flags; project is open-source style, no deps |
| 2. **vi editor** & basic commands | Development/demo done in vi; `README` section "editing config in vi"; commands `ls -l`, `stat`, `chmod`, `touch`, `cat`, `kill`, `ps` used in demos and tests |
| 3. File system, directories, **I/O redirection, pipes, processes** | Directory walk; daemon stdout/stderr redirected with `open+dup2`; anonymous **pipe** between daemon and worker; `report.sh` pipelines (`grep \| sed \| awk \| sort \| uniq -c`); process lifecycle (`fork`, `setsid`, `wait`) |
| 4. **Shell scripting & filters:** grep, sed, awk, while/until loops, traps | `report.sh` (grep/sed/awk), `launch.sh` (`until` waits for readiness, `while` waits for exit, `trap` cleanup), `integrityctl` (`trap`, functions, `read -t`) |
| 5. **Standard I/O** | Snapshot and change log written with `fopen/fprintf/fgets/fflush/fclose` |
| 6. **System calls:** low-level I/O, directories, **inodes**, **process, signals & interrupts** | Hashing with `open/read/close`; `opendir/readdir`, `lstat` (inode, ctime, nlink); `fork/execve/waitpid`; `sigaction`, `kill`, `alarm`/SIGALRM as timer interrupt; `mkfifo` |

Honesty note for the report: `inotify`, `poll`, `fnmatch`, `getopt_long` and SHA-256 are *outside* the syllabus (marked small diversions); the system also works in pure `scan` mode that uses only syllabus primitives (D-06).

---

## 3. Architecture

### 3.1 Pipeline `[ORIGINAL]`
A single pipeline with two side entry points that can act on it independently:
```
 Directory Scanner  ── reads path, metadata, hash for every file
        │
        ▼
 Snapshot Store ───── durable record of the last known-good state
        │
 Event Listener ───── wakes only when the file system actually changes   ┐
 Rescan signal  ───── forces an immediate full check                      ├─► Comparison Engine ─► Change Log ─► Report Generator
 Control pipe   ───── accepts pause / resume / status / add-path / …      ┘   (metadata + hash vs snapshot)  (timestamped, append-only)  (plain-language summary on request)
```

### 3.2 Process model `[ADDED]` (D-07)
One **single-threaded** daemon process with a `poll()` event loop. No threads. At most **one** short-lived forked child at a time (deep-verify worker) plus transient hook children.

```
                         ┌───────────── poll() ─────────────┐
 self-pipe (signals) ───►│                                  │
 control FIFO ──────────►│          daemon.c              │──► compare ──► logger (chain, fsync)
 inotify fd ────────────►│   state: PathMap<Record>,        │──► snapshot (atomic save)
 worker pipe (optional) ►│   roots, wd↔path maps, flags     │──► hook (fork/exec)
                         └──────────────────────────────────┘
 SIGALRM tick ─► self-pipe ─► periodic scan        SIGCHLD ─► self-pipe ─► waitpid()
```

### 3.3 Detection modes `[REVISED]` (D-06)
| Mode | Mechanism | When |
|---|---|---|
| `scan` | `alarm()`/SIGALRM timer → metadata scan; hash only when the stat tuple changed, plus a **deep hash-everything** pass every `deep_every` ticks | syllabus-only core; works anywhere |
| `watch` | inotify events only (+ startup reconciliation + overflow rescans) | real time |
| `hybrid` (**default**) | both: inotify for instant detection, periodic scan as safety net (catches missed events, raw writes, watch-limit overflow) | production-like |

Both modes feed the **same** comparison engine (`reconcile_paths`).

### 3.4 Startup sequence `[ADDED]`
1. Parse CLI → load config → create run dir (mode 0700) with `reply/` subdir.
2. Pid-file check: if `integrityd.pid` names a live process (`kill(pid,0)` succeeds and `/proc/<pid>/comm` is `integrityd`) → exit "already running" (exit 3); stale file → remove.
3. Daemonize (unless `--foreground`) — 5.9. Write pid file (mode 0644).
4. Open the change log (continue sequence number and hash chain from its last valid line).
5. Load snapshot (verify trailer checksum). Missing → **initial baseline**: full deep scan, no alerts, save, log `DAEMON_START baseline=initial`. Corrupt → rename to `snapshot.db.corrupt.<unix_ts>`, log `ERROR snapshot_corrupt`, then do an initial baseline. Valid → **startup reconciliation** per `startup_check` (`deep` default = hash everything; `meta`; `none`) emitting normal events with detail `offline=1`.
6. Install signal handlers + self-pipe; create control FIFO; set up inotify (if mode ≠ `scan`); arm `alarm(scan_interval)`; enter the event loop.

### 3.5 Event loop (`daemon.c`) `[ADDED]`
```
for (;;) {
   timeout = pending_batch ? ms_until(batch_deadline) : -1;
   n = poll([selfpipe_r, control_fd, inotify_fd?, worker_fd?], timeout);
   if (n < 0 && errno == EINTR) continue;
   if selfpipe readable   → drain bytes; for each signal byte → handle_signal(sig)
   if control readable    → read available bytes, split into lines, handle_command(line)
   if inotify readable    → read all events, update pending path set, add/remove watches
   if worker pipe readable→ read records, compare, emit events;  EOF → worker done
   if pending_batch and now ≥ batch_deadline → reconcile_paths(pending); clear pending
   if shutdown_flag → break
}
cleanup(); // save snapshot, log DAEMON_STOP, remove pid/fifo, exit(0)
```
**Debounce (D-08):** the first inotify event opens a batch window of `batch_ms` (default 200 ms) that is extended by each new event up to a hard cap of `batch_max_ms` (default 1000 ms). Within a batch each path is checked once (`lstat` + hash if regular file).

### 3.6 Signals (final table) `[REVISED]` (D-09)
Installed with `sigaction` (no `SA_RESTART`; `EINTR` handled). The handler body is exactly: `int e=errno; unsigned char b=(unsigned char)sig; write(selfpipe_w,&b,1); errno=e;` (write end is non-blocking, close-on-exec).
| Signal | Effect |
|---|---|
| `SIGTERM`, `SIGINT` | graceful shutdown (finish current batch, save snapshot, log `DAEMON_STOP`, remove pid file and FIFO, exit 0) |
| `SIGHUP` | reload configuration (not the root set; see 5.10) |
| `SIGUSR1` | **immediate full rescan** (the original "single signal to force a full check") |
| `SIGUSR2` | write a `STATUS` INFO line to the log |
| `SIGALRM` | periodic tick: run scan; re-arm `alarm(scan_interval)` |
| `SIGCHLD` | reap children with `waitpid(-1, &st, WNOHANG)` loop; log abnormal worker exit |
| `SIGPIPE` | ignored |
`kill -USR1 $(cat ~/.integrityd/integrityd.pid)` is the documented demo of "on-demand rescan".

---

## 4. Data Formats `[ADDED]` (D-10)

### 4.1 Record (in memory and in the snapshot)
| Field | Type | Notes |
|---|---|---|
| `type` | char | `f` regular file, `d` directory, `l` symlink, `o` other (fifo/socket/device) |
| `path` | string | absolute, canonical root + relative part; no trailing slash |
| `dev`, `ino` | uint64 | from `lstat` |
| `mode` | uint32 | `st_mode & 07777` (includes setuid/setgid/sticky), printed octal |
| `uid`, `gid` | uint32 | |
| `nlink` | uint64 | |
| `size` | int64 | |
| `mtime_ns`, `ctime_ns` | int64 | from `st_mtim`, `st_ctim` (nanoseconds since epoch) |
| `sha` | 64-hex or `-` | SHA-256 of contents (regular files); of the link-target string (symlinks); `-` for dirs/others/unreadable |
| `target` | string or `-` | `readlink` result for symlinks |
Note: `ctime` cannot be set by user-space (`utimensat` changes mtime/atime only), so a content edit that restores size and mtime **still changes ctime** — this is the cheap tamper tell used by scan mode (D-11). The deep hash covers the remaining cases.

### 4.2 Snapshot file `snapshot.db`
Text, UTF-8-agnostic (bytes), `\n`-terminated lines:
```
#INTEGRITYD-SNAPSHOT v1
#ROOT <escaped absolute root path>          (one line per watched root; roots are state, see 5.10)
#CREATED <YYYY-MM-DDTHH:MM:SSZ>
<type>\t<path>\t<dev>\t<ino>\t<mode_octal>\t<uid>\t<gid>\t<nlink>\t<size>\t<mtime_ns>\t<ctime_ns>\t<sha|->\t<target|->
...                                          (sorted by path, bytewise)
#END <record_count> <sha256 hex of every byte before this line>
```
- **Escaping (`escape.c`):** percent-encode `%`, TAB, LF, CR, bytes < 0x20 and 0x7F as `%XX`; additionally `;` → `%3B` and `=` → `%3D` inside log `detail` values. `unescape` is its exact inverse. Unit-tested for round trip on random bytes.
- **Write procedure:** build in memory/stream to `snapshot.db.tmp` with `fopen/fprintf`, `fflush`, `fsync(fileno)`, `fclose`, then `rename()` over `snapshot.db`, then `fsync` the directory. Loading verifies the `#END` count and checksum; any mismatch → corrupt (3.4 step 5).
- **Flush policy:** snapshot saved after any scan that changed records, at most every `snapshot_flush_secs` (default 5) after event batches, and always at shutdown, `REBASELINE`, `ADD`, `REMOVE`.

### 4.3 Change log `change.log`
One record per line, TAB-separated, UTF-8-agnostic:
```
<seq>\t<timestamp>\t<severity>\t<event>\t<escaped path or ->\t<detail k=v;k=v or ->\t<chain>
```
- `seq`: uint64, starts at 1, +1 per record, continues across restarts (read from the last valid line) and rotations.
- `timestamp`: UTC `YYYY-MM-DDTHH:MM:SS.mmmZ`.
- **Severity / event table:**
| Event | Severity | Meaning / typical detail keys |
|---|---|---|
| `CREATED` | WARN | new path not in baseline (`size`,`sha`,`mode`) |
| `MODIFIED` | ALERT | content changed (`old_sha`,`new_sha`,`old_size`,`new_size`, `stealth=1` if the stat tuple was unchanged, `basis=meta` if hash unavailable) |
| `DELETED` | ALERT | path gone (`old_sha`,`old_size`) |
| `PERMS` | ALERT | mode/uid/gid changed (`old_mode`,`new_mode`,`old_uid`,`new_uid`,`old_gid`,`new_gid`) — **distinct from content edits** |
| `TYPE_CHANGED` | ALERT | file↔dir↔symlink (`old_type`,`new_type`) |
| `MOVED` | WARN | same inode and same hash seen at a new path (`from=<old path>`; `path` field = new path) |
| `REPLACED` | INFO | same content, new inode (editor "atomic save") |
| `UNREADABLE` | WARN | open/readdir denied (`errno`) — logged once until the condition clears |
| `UNSTABLE` | WARN | file kept changing while being hashed (3 retries) |
| `ERROR` | WARN | internal/IO errors (`what`,`errno`) |
| `DAEMON_START`, `DAEMON_STOP`, `SCAN_END`, `PAUSED`, `RESUMED`, `REBASELINE`, `WATCH_ADDED`, `WATCH_REMOVED`, `CONFIG_RELOAD`, `OVERFLOW_RESCAN`, `LOG_ROTATE`, `STATUS` | INFO | system events |
Extra detail on any event produced while paused or at startup: `during_pause=1` / `offline=1`.
- **Hash chain (tamper evidence, D-12):** `chain = SHA256(prev_chain ‖ "\n" ‖ <line text before the final TAB+chain>)` as 64 hex; the first record of a new log uses 64 zeros; rotation continues the chain (4.4).
- **Open mode:** `open(path, O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC, 0600)` wrapped by `fdopen`; each record is one `fprintf` followed by `fflush` and (if `log_fsync=1`, default) `fsync`. This keeps the original "buffered I/O" while guaranteeing durability.
- `integrityd verify-log` recomputes the chain over `change.log.N … change.log` and prints the first broken sequence number, or `OK <records> head=<chain>`.

### 4.4 Rotation
When `change.log` exceeds `log_max_bytes` (default 10 MiB): append `LOG_ROTATE` (INFO) as the last record, `rename` `change.log`→`change.log.1` (shifting existing `.N` up to `log_keep` = 5, deleting the oldest), start a new `change.log` whose first record is `LOG_ROTATE cont=1`, continuing `seq` and `chain`.

### 4.5 Configuration `integrityd.conf`
`key=value`, `#` comments, repeated keys allowed where noted. All paths absolute (resolved with `realpath` at load).
| Key | Default | Meaning |
|---|---|---|
| `watch` (repeatable) | — | **seed** roots, used only when no snapshot exists or to add roots not yet known |
| `exclude` (repeatable) | `*.swp` `*.swx` `*~` `.#*` | `fnmatch(3)` patterns, matched against the basename and the root-relative path; trailing `/` = directories only; excluded dirs are neither scanned nor watched |
| `mode` | `hybrid` | `scan` \| `watch` \| `hybrid` |
| `scan_interval` | 60 | seconds between periodic scans (SIGALRM) |
| `deep_every` | 10 | every Nth periodic scan hashes **all** files |
| `startup_check` | `deep` | `deep` \| `meta` \| `none` |
| `batch_ms` / `batch_max_ms` | 200 / 1000 | inotify debounce |
| `snapshot_flush_secs` | 5 | |
| `log_fsync` | 1 | |
| `log_max_bytes` / `log_keep` | 10485760 / 5 | rotation |
| `hash_max_bytes` | 0 | 0 = hash files of any size |
| `one_filesystem` | 1 | do not descend into other mounts (compare `st_dev`) |
| `max_depth` | 64 | directory depth limit |
| `deep_verify_in_worker` | 1 | run deep passes in a forked worker (5.7) |
| `alert_hook` | (none) | absolute path of an executable (5.8) |
| `hook_min_severity` | `ALERT` | `WARN` \| `ALERT` |
| `hook_rate_per_sec` | 10 | burst limit |

### 4.6 Control protocol `[ADDED]` (D-13)
- Files in the run dir: `control.fifo` (mode 0600), `reply/` (mode 0700), `integrityd.pid`, `daemon.out`.
- Server opens the FIFO `O_RDONLY|O_NONBLOCK` **and** a second `O_WRONLY` descriptor kept open (so the read side never sees EOF and `poll` does not spin). It accumulates bytes, splits on `\n`, ignores lines > 4096 bytes. Client writes of ≤ `PIPE_BUF` bytes are atomic.
- **Request line:** `<req-id> <reply-fifo-path|-> <COMMAND> [arg ...]` — fields separated by single spaces; any argument containing space/`%`/control bytes is percent-escaped.
- **Reply:** server opens the client's reply FIFO `O_WRONLY|O_NONBLOCK` (if no reader → drop silently), writes lines `OK <text>` or `ERR <code> <message>`, then a line containing only `.` and closes. Never blocks the loop.
- **Commands (final):**
| Command | Effect |
|---|---|
| `PING` | `OK pong` |
| `VERSION` | version string |
| `STATUS` | one line `state=running\|paused mode=hybrid roots=N records=N watches=N uptime_s=N last_scan=<ts> last_scan_ms=N alerts_total=N log_seq=N pid=N worker=idle\|running` |
| `PAUSE` | stop emitting alerts and stop periodic scans; keep draining inotify into a pending set; log `PAUSED`. **A pause never creates a blind spot:** |
| `RESUME` | performs a full scan (and processes the pending set); changes found are reported with `during_pause=1`; log `RESUMED` |
| `RESCAN [path]` | immediate metadata scan of everything or one subtree |
| `DEEPVERIFY` | immediate deep (hash-all) pass |
| `ADD <path>` | validate (exists, is a directory or file, not inside an existing root; if it contains existing roots they are merged into it), baseline it silently, add watches, persist roots, log `WATCH_ADDED` |
| `REMOVE <path>` | remove a root, drop its records and watches, persist, log `WATCH_REMOVED` |
| `LIST` | the roots, one per line |
| `REBASELINE [path]` | adopt the current disk state as baseline **without alerts**; log `REBASELINE` with counts |
| `RELOAD` | same as SIGHUP |
| `TAIL <n>` | last n (≤ 200) log lines |
| `SHUTDOWN` | same as SIGTERM |
Unknown command → `ERR 400 unknown command`; bad argument → `ERR 422 <why>`; internal → `ERR 500 <why>`.

---

## 5. Subsystem Specifications

### 5.1 Scanner (`scanner.c`) `[ORIGINAL][REVISED]`
- Recursive walk with `opendir/readdir/closedir`; skip `.` and `..`; **`lstat` only** (never follow symlinks → no loops); `S_ISDIR/S_ISREG/S_ISLNK` classification; non-regular, non-dir, non-link → type `o` and **never opened** (opening a FIFO would block forever).
- Does not cross filesystems when `one_filesystem=1` (compare `st_dev` with the root's); depth limit `max_depth`.
- Applies excludes with `fnmatch(basename)` and `fnmatch(relative path)`.
- `readdir` failure/`EACCES` on a directory → record the directory itself, log `UNREADABLE` once (a per-run set remembers which), continue.
- Output: an array of `Record` (a `Vec`, without hashes) sorted by path with `qsort`; hashing is a separate step (5.2) so scan mode can skip it.
- Original behaviour retained: emits `path`, inode, size, mtime, permission bits for every entry (the original fragment printed `path|inode|size|mtime|mode`).

### 5.2 Hashing (`hashing.c`, `sha256.c`) `[REVISED]` (D-14)
- SHA-256 per FIPS 180-4, streaming (`init/update/final`). Unit vectors: `""` → `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`; `"abc"` → `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`; 1 000 000 × `'a'` → `cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0`; lengths 55/56/63/64/65 and a 100 MB file compared to `sha256sum`.
- `hash_file(path)`: `fd = open(path, O_RDONLY|O_NOFOLLOW|O_CLOEXEC)`; `fstat(fd)` must show a regular file (else treat as `o`); read in 64 KiB chunks with `read()`; `fstat` again afterwards — if `size`, `mtime` or `ctime` changed during hashing, retry (max 3) then emit `UNSTABLE` and skip. `EACCES`/`EPERM` → `UNREADABLE` once, hash `-`. (Replaces the original 64-bit djb2 / byte-at-a-time `fgetc`, which is non-cryptographic and slow — D-14.)

### 5.3 Snapshot store (`snapshot.c`, `hashtab.c`) `[ORIGINAL][ADDED]`
- In memory: the project's single chained hash table (`hashtab.c`: FNV-1a 64-bit, grow ×2 at load 0.75) mapping path → `Record *`. Saving collects the records into a `Vec` and `qsort`s them by path for a deterministic file.
- Save/load per 4.2. Memory target ≤ 300 bytes + path per record.

### 5.4 Comparison engine (`compare.c`) `[ORIGINAL][ADDED]` (D-15)
`reconcile_paths(<set of paths>)` and `full_compare(scan result)` both call `classify(old, new, deep_flag)`:
1. old absent, new absent → nothing.
2. old absent → `CREATED`. new absent → `DELETED`.
3. type differs → `TYPE_CHANGED`.
4. **Regular file:** 
   a. both hashes known and different → `MODIFIED` (`stealth=1` if `(dev,ino,size,mtime_ns,ctime_ns)` are all equal);
   b. hash unknown on a side → compare `(size,mtime_ns,ctime_ns)`; differ → `MODIFIED basis=meta`;
   c. `mode`/`uid`/`gid` differ → `PERMS` (emitted **in addition** to a, if both apply);
   d. otherwise if `(dev,ino)` differ → `REPLACED`;
   e. otherwise refresh `mtime/ctime/nlink` silently.
5. **Symlink:** target hash differs → `MODIFIED`; uid/gid differ → `PERMS`. **Directory / other:** mode/uid/gid differ → `PERMS`.
6. **Fast path (scan, non-deep):** if the whole stat tuple `(dev,ino,size,mtime_ns,ctime_ns,mode,uid,gid,nlink)` equals the baseline → skip classification and hashing entirely. Event-triggered checks and deep passes always hash.
7. **MOVED pairing** (after classifying a batch/scan): a `DELETED` and a `CREATED` regular file with equal `(dev,ino)` **and equal sha** merge into one `MOVED`. Equal inode with different content is *not* paired (inode reuse after delete+create is common).
8. **Baseline policy (D-16):** after reporting, the baseline advances to the new state (each change is reported once). Exception: nothing advances for an `UNSTABLE`/`UNREADABLE` path until it is checkable.
9. Table-driven unit tests cover every row above.

### 5.5 Watcher (`watcher.c`) `[ORIGINAL][REVISED]` (D-17)
- `inotify_init1(IN_CLOEXEC|IN_NONBLOCK)`. Mask per directory: `IN_CREATE|IN_DELETE|IN_MODIFY|IN_ATTRIB|IN_CLOSE_WRITE|IN_MOVED_FROM|IN_MOVED_TO|IN_DELETE_SELF|IN_MOVE_SELF|IN_DONT_FOLLOW|IN_ONLYDIR`.
- **Recursive:** one watch per directory. Hand-written `wd→dirpath` and `dirpath→wd` hash maps. Event path = `dirpath + "/" + name`.
- New directory (`IN_CREATE|IN_ISDIR` or `IN_MOVED_TO|IN_ISDIR`): immediately add a watch **and** walk the subtree, adding every entry to the pending set (closes the create-then-populate race). `IN_IGNORED`/`IN_DELETE_SELF`: remove mappings.
- Read buffer: `_Alignas(struct inotify_event) char buf[64*1024]`; loop `read` until `EAGAIN`; advance by `sizeof(inotify_event)+len`.
- `IN_Q_OVERFLOW` → log `OVERFLOW_RESCAN` and schedule a full metadata scan. `ENOSPC` from `inotify_add_watch` (watch limit) → log `ERROR what=watch_limit`, mark that root *degraded* (scan-only fallback) and continue.
- Every event only adds its path to the pending set (debounce 3.5); classification happens in `reconcile_paths`.

### 5.6 Periodic scan (`daemon.c`) `[ADDED]`
`SIGALRM` → tick counter++ → if `tick % deep_every == 0` deep pass else metadata pass; `alarm(scan_interval)` re-armed after the pass. Deep pass runs in the worker if `deep_verify_in_worker=1` and `records > 2000`, else inline. Log `SCAN_END` (INFO) with `files`,`changes`,`ms` for every scan.

### 5.7 Deep-verify worker (`worker.c`) `[ADDED]` (D-18) — demonstrates `fork`, `pipe`, `wait`
- Parent: `pipe2(fds, O_CLOEXEC)`, `fork()`. Only one worker at a time.
- Child: restore default signal handlers, close control/inotify fds, iterate the (copy-on-write) baseline paths, hash each file, write lines `<escaped path>\t<sha>\t<size>\t<mtime_ns>\t<ctime_ns>\n` with `write()` to the pipe, finish with `#DONE <count>\n`, `_exit(0)`.
- Parent: read end non-blocking, in the `poll` set; parse lines; for each result **re-`lstat` the path**: if the stat differs from what the child saw, skip (the event/next scan will handle it); else compare with baseline via `classify` (deep semantics). `SIGCHLD` → `waitpid`; non-zero exit or missing `#DONE` → log `ERROR what=worker`.
- Benefit to describe in the report: the main loop stays responsive to events and control commands during large hash passes.

### 5.8 Alert hook (`hook.c`) `[ADDED]` (D-19) — demonstrates `fork`/`execve`
- Config `alert_hook=/abs/path`. Validated at load/RELOAD: absolute, regular file, executable, owned by the daemon's uid or root, **not** group/world-writable (`mode & 022 == 0`).
- On each event with severity ≥ `hook_min_severity`: rate limit `hook_rate_per_sec` (excess counted and reported once per second as `hook_dropped=N`); `fork`; child: `setsid`, `dup2` `/dev/null` to 0/1/2, `execve(hook, argv=[hook, EVENT, PATH, SEVERITY, SEQ], envp=["INTEGRITYD_DETAIL=<escaped detail>", "PATH=/usr/bin:/bin"])` — **never** through a shell and never with file names in a command string. Parent does not wait (reaped via SIGCHLD).

### 5.9 Daemonisation (`daemonize.c`) `[ADDED]` (D-20)
```
fork(); parent _exit(0)                  # not a process-group leader
setsid()                                 # new session, no controlling terminal
signal(SIGHUP, SIG_IGN)
fork(); parent _exit(0)                  # can never re-acquire a tty
umask(077); chdir("/")                   # all paths in this project are absolute
close all inherited fds ≥ 3 (loop to sysconf(_SC_OPEN_MAX) or /proc/self/fd)
fd = open("/dev/null", O_RDWR); dup2(fd, 0)
fd = open(<rundir>/daemon.out, O_WRONLY|O_CREAT|O_APPEND, 0600); dup2(fd, 1); dup2(fd, 2)   # I/O redirection
write pid file; (stdout/stderr unbuffered for diagnostics)
```
`--foreground` skips all of this (used by tests and debugging). Stop = `SIGTERM`.

### 5.10 Configuration reload and roots (`config.c`) `[ADDED]` (D-21)
- **Roots are state, owned by the snapshot** (`#ROOT` lines). `watch=` in the config only seeds roots that are not yet known. `ADD`/`REMOVE` change the snapshot's root list. Removing a root permanently is done with `REMOVE` (or `integrityctl remove`), not by editing the config.
- `RELOAD`/`SIGHUP` re-reads everything else (intervals, excludes, hook, log params, mode changes take effect at the next loop iteration; switching to/from inotify adds/removes watches).

### 5.11 Shell layer `[ORIGINAL][REVISED]`
**`scripts/launch.sh start|stop|restart|status`** (syllabus: `while`, `until`, functions, `trap`)
- `start`: checks no live pid; runs `integrityd daemon "$@"`; `until [ -s "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")"; do sleep 0.1; done` with a 5 s cap; prints `started pid N` or a diagnostic from `daemon.out`.
- `stop`: `kill -TERM`, `while kill -0 "$pid" 2>/dev/null; do …; done` up to 10 s, then reports (never `kill -9` automatically).
- `status`: pid, uptime, and `integrityctl status` output.
- `trap 'cleanup' EXIT INT TERM` removes temp files.

**`scripts/integrityctl <command> [args]`** — builds the request line, creates a reply FIFO `mkfifo "$RUNDIR/reply/$$.$RANDOM"`, `trap 'rm -f "$reply"' EXIT INT TERM`, opens it read-write with `exec 3<>"$reply"` (so opening never blocks), writes the request to `control.fifo` (single `printf` ≤ 4096 bytes), then `while read -t 3 -r -u 3 line; do [ "$line" = "." ] && break; echo "$line"; done`; prints `integrityctl: no reply (daemon not running?)` and exits 1 on timeout. Exit code 0 on `OK`, 2 on `ERR`. Subcommands map 1:1 to Section 4.6 (`status pause resume rescan deepverify add remove list rebaseline reload tail shutdown ping`), plus `kill-rescan` which just does `kill -USR1 "$(cat pidfile)"`.

**`scripts/report.sh`** — see Section 8.

---

### 5.12 Small C helpers (`util.c`, `hashtab.c`) `[ADDED]` (D-23)
C has no growable string/array or hash table in libc, so exactly these helpers are written (each < 80 lines, unit-tested); everything else uses libc (`qsort`, `bsearch`, `snprintf`, `fnmatch`, `realpath`, `getopt_long`).
- `Str`: `{char *p; size_t len, cap;}` with `str_init/str_append/str_appendf/str_free`; always NUL-terminated.
- `Vec`: `{void **items; size_t n, cap;}` growable array of pointers with `vec_push/vec_free`.
- `HashTab`: chained hash table, string key → `void *`, `ht_put/ht_get/ht_del/ht_iter`, FNV-1a 64-bit, grows ×2 at load 0.75. Used for path→`Record *`, `wd→dir`, `dir→wd`, per-run "already reported unreadable" sets.
- `xmalloc/xrealloc/xstrdup`: abort with `integrityd: out of memory` on failure.
- All strings handed to the kernel are built with `Str`/`snprintf` sized from `strlen`; never a fixed `char[1024]`.

---

## 6. Command-Line Interface `[ADDED]` (D-22)
```
integrityd daemon   [--rundir D] [--config F] [--watch DIR ...] [--mode hybrid|scan|watch] [--foreground]
integrityd baseline --watch DIR ... [--rundir D]      # create/replace snapshot without starting a daemon (no alerts)
integrityd check    [--rundir D] [--deep]             # one-shot: compare disk vs snapshot, print events, exit 0 clean / 1 changes / 2 error
integrityd hash     FILE...                           # prints "<sha256>  <file>" (same format as sha256sum)
integrityd verify-log [--rundir D | --log FILE]       # verify hash chain
integrityd version
```
Parsed with `getopt_long` (libc). Defaults: `--rundir ~/.integrityd` (or `/var/lib/integrityd` for uid 0). `check` and `baseline` reuse the same scanner/compare/log code (no daemon needed) — used by tests and classroom demos; `check` writes events to stdout in log format and does not modify the snapshot unless `--update` is given.


---

## 7. Errors, Edge Cases and Security `[ADDED]` (D-24)

### 7.1 Exit codes and messages
`0` success / clean · `1` changes found (`check`) or general error · `2` usage or protocol error · `3` already running · `4` corrupt snapshot/log detected (`verify-log` failure). Diagnostics: `integrityd: <message>` on stderr (or in `daemon.out` when detached) and an `ERROR` log record where relevant.

### 7.2 Edge-case rules (each has a test in Section 11)
| Situation | Required behaviour |
|---|---|
| Symlink (incl. loops, dangling) | `lstat` only, never followed; target string hashed and tracked |
| FIFO / socket / device inside the tree | type `o`; recorded, **never opened** |
| Unreadable file or directory | `UNREADABLE` once; cleared and re-armed when it becomes readable |
| File changes while being hashed | retry ≤ 3 → `UNSTABLE`, baseline not advanced |
| Rename inside the tree | `MOVED` (same inode + hash) |
| Editor "atomic save" (write temp + rename) | content change → `MODIFIED` once (debounce) plus `REPLACED` INFO when only the inode changed |
| Delete + create reusing an inode | not paired as `MOVED` unless the hash is equal |
| File replaced by directory (or reverse) | `TYPE_CHANGED` |
| Watched root deleted or renamed | `DELETED` for all its records, root marked `missing`, watch retried on each periodic scan |
| New directory populated before its watch exists | covered by subtree walk on `IN_CREATE|IN_ISDIR` (5.5) |
| inotify queue overflow / watch limit | `OVERFLOW_RESCAN` / degraded root with scan-only fallback (5.5) |
| Paths containing spaces, tabs, newlines, non-UTF-8 bytes | escaped in snapshot/log/control protocol (4.2) |
| Very large file | hashed in 64 KiB chunks; `hash_max_bytes` optional cap (hash recorded as `-`, compare by size/mtime/ctime) |
| Clock changes / mtime in the future | ctime/hash decide; timestamps only used for display |
| Snapshot or log corrupted | 3.4 step 5; `verify-log` pinpoints the first bad sequence number |
| Disk full while writing log/snapshot | `ERROR what=io` to stderr/`daemon.out`; snapshot write is atomic (old file survives); daemon keeps running |
| Second daemon on the same run dir | exit 3 via pid-file check |
| Control FIFO flooded or malformed lines | lines > 4096 bytes ignored; unknown commands answered `ERR 400` |

### 7.3 Security notes
Run dir mode 0700, FIFO 0600, log 0600 (the original `0666` FIFO is rejected, D-13). The alert hook is validated and never run through a shell (5.8). Path arguments to `ADD`/`REMOVE` are canonicalised with `realpath` and must be absolute. Signal handlers are async-signal-safe (3.6). Threat model limits in 1.3 are stated in the report.

---

## 8. Reporting Layer (`scripts/report.sh`) `[ORIGINAL][REVISED]`

Original behaviour kept: turn the raw log into a one-line, human-readable summary using `grep`/`awk`. Original reference script:
```bash
#!/bin/bash
LOG="data/change.log"
modified=$(grep -c "MODIFIED" "$LOG")
created=$(grep -c "CREATED"  "$LOG")
deleted=$(grep -c "DELETED"  "$LOG")
awk -v m="$modified" -v c="$created" -v d="$deleted" \
    'BEGIN { printf "Report: %d modified, %d created, %d deleted\n", m, c, d }'
```
**Revised interface** (the original greps would also match file *names* containing "MODIFIED" and ignore permission events and rotated logs — fixed by matching the 4th TAB-separated field):
```
report.sh [--rundir D] [--since "YYYY-MM-DD[ HH:MM]"] [--until ...] [--format oneline|text|html] [--events]
```
- Default `oneline`: `Report: 3 modified, 2 created, 1 deleted, 1 permission change, 0 moved (since 2026-10-31 09:00)`.
- `text`: the one-liner, then a per-event-type count table (`awk -F'\t'` + `sort | uniq -c`), then the 20 most recent ALERT lines rendered with `sed` as `[ts] MODIFIED /path (size 120 -> 188, content changed)`; plus the top 5 most-changed paths (`sort | uniq -c | sort -rn | head`).
- `html` (optional, **no JavaScript framework**): a single static file produced with `printf` here-docs; table of counts and recent alerts; used for the demo; written to `<rundir>/report.html`.
- Reads `change.log.N … change.log` in order (`ls -v`), filters the date range with `awk` string comparison on the ISO timestamp, ignores `#` lines and INFO events unless `--events`.
- Syllabus constructs deliberately shown: `grep`, `sed`, `awk`, pipes, `while read`, functions, `case`, `trap` for temp-file cleanup, command-line option loop.

---

## 9. Milestones and Definition of Done `[ADDED]` (D-25)
Original seven build stages are preserved as the first seven milestones (each runnable and demoable); the added hardening follows.

| S | Deliverable (original stage → added work) | Definition of Done |
|---|---|---|
| S0 | Skeleton: Makefile, `util.c`, `hashtab.c`, test macros | `make test` runs; helper unit tests pass |
| S1 | **Directory Scanner** (original stage 1): `opendir/readdir/lstat`, recursion, excludes, `integrityd check --scan-only` prints records | scans a fixture tree; symlinks/FIFOs handled; sorted output |
| S2 | **Metadata & Hash Capture** (stage 2): `sha256.c` + `hashing.c`, `integrityd hash` | NIST vectors + `sha256sum` equality on random files; unstable-file retry tested |
| S3 | **Snapshot Persistence** (stage 3): `escape.c`, `snapshot.c`, `baseline` command | save/load round trip incl. weird names; corrupted file detected; atomic write |
| S4 | **Comparison & Alert Logic** (stage 5, done early so it can be tested offline): `compare.c`, `logger.c` with hash chain, `check` command | table-driven classify tests; stealth-edit (size+mtime restored) detected; `verify-log` OK and detects tampering |
| S5 | **Real-Time Event Listener** (stage 4): `watcher.c` recursive inotify, debounce, overflow handling, `daemon.c` loop with `--foreground` | create/modify/delete/chmod/rename/new-subtree events logged within ≈ 1 s |
| S6 | **Control Pipe & Rescan Signal** (stage 6): `signals.c` self-pipe, `control.c`, `scripts/integrityctl`, SIGUSR1/SIGHUP/SIGALRM/SIGCHLD | `integrityctl status/pause/resume/add/remove/rescan/rebaseline` work live; `kill -USR1` rescans |
| S7 | **Reporting Layer** (stage 7): `report.sh`, `launch.sh` | all three formats produce correct counts on a scripted log |
| S8 | `daemonize.c`, pid file, startup reconciliation (offline changes), log rotation | start/stop via `launch.sh`; change files while stopped → reported on start |
| S9 | `worker.c` (fork + pipe deep verify), `hook.c` (fork/execve) | worker survives kill; hook receives args/env; rate limit works |
| S10 | Polish: README, demo script, `make asan` (ASan+UBSan) clean, DECISIONS.md | demo script runs end to end |
**Minimum complete submission = S0–S7** (everything the original handbook promised). Recommended target = S0–S10.

---

## 10. Original Verification Checklist `[ORIGINAL]` (each item maps to tests in Section 11)
1. **Creation is detected** — a new file appears and is logged as new (`CREATED`; T05).
2. **Content edits are caught, even when disguised** — overwriting a file's bytes while restoring size and timestamp still trips the hash check (`MODIFIED stealth=1`; T07).
3. **Deletion is detected** — a removed file is reported, not silently dropped (T08).
4. **Permission-only changes are flagged separately** — a `chmod` with unchanged content is distinguished from a content edit (`PERMS`; T09).
5. **Nested directories are covered** — subfolders are traversed and watched, not just the top level (T10, T11).
6. **The control pipe responds live** — a command sent from another terminal changes daemon behaviour without a restart (T14–T16).
Added checks: signal-triggered rescan (T17), pause/resume without blind spot (T15), restart persistence and offline changes (T18), log durability and tamper detection (T19, T20).

---

## 11. Test Plan `[ADDED]` (D-26)
Unit tests (`tests/unit`, own `CHECK` macros): SHA-256 vectors (4.2/5.2), escape/unescape round trip on random bytes, `HashTab`/`Str`/`Vec`, snapshot save/load, `classify` table, log chain, config parser. Integration tests (`tests/integration/Tnn_*.sh`, run by `make test`, each in a temp run dir with `--foreground` daemon or `check`):
| ID | Scenario |
|---|---|
| T01 | baseline then `check` on unchanged tree → exit 0 |
| T02 | scanner handles symlink loop, dangling link, FIFO in tree (no hang) |
| T03 | weird file names (space, tab, newline, `%`, `;`, `=`) round-trip through snapshot, log, control |
| T04 | hash equals `sha256sum` for files of 0, 1, 55, 56, 64, 65, 100 000 bytes and 100 MB |
| T05 | new file → `CREATED` |
| T06 | append to file → `MODIFIED` with new size/hash |
| T07 | **stealth edit**: change bytes, restore size and mtime with `touch -r` → `MODIFIED stealth=1` (deep) |
| T08 | delete file → `DELETED` |
| T09 | `chmod` only → `PERMS`, no `MODIFIED` |
| T10 | change in a nested subdirectory 3 levels deep |
| T11 | new directory created and immediately populated → files reported (race) |
| T12 | rename file → `MOVED`; delete+create different content same name → `DELETED`+`CREATED` (or `MODIFIED`) not `MOVED` |
| T13 | editor-style atomic save → one `MODIFIED` (debounce) |
| T14 | `integrityctl status` / `ping` / `list` replies correctly |
| T15 | `pause` → change files → `resume` → changes reported with `during_pause=1` |
| T16 | `add` a new root live, edit inside it → reported; `remove` it → silence |
| T17 | `kill -USR1` triggers `SCAN_END` and finds a change the watcher was told to miss (mode `scan`) |
| T18 | stop daemon, change files, start → offline changes reported; snapshot survives |
| T19 | tamper with a log line → `verify-log` reports the exact sequence number; truncate last line → handled |
| T20 | log rotation keeps chain and sequence continuous across `change.log.1` |
| T21 | corrupt `snapshot.db` → renamed `.corrupt.*`, re-baseline, `ERROR` logged |
| T22 | worker deep verify: kill worker mid-run → `ERROR what=worker`, daemon alive |
| T23 | alert hook invoked with correct args; non-executable or world-writable hook rejected |
| T24 | `report.sh` outputs match counts from a scripted log (oneline/text/html) |
Quality gates: `make asan`, zero warnings, `shellcheck`-clean scripts if `shellcheck` is installed (optional).

---

## 12. Demo Plan `[ADDED]`
1. `./scripts/launch.sh start --watch ~/demo` → show `ps`, pid file, `daemon.out`.
2. In another terminal: `echo hi >> a.txt`, `touch new`, `rm old`, `chmod 600 secret` → show `tail -f change.log` and `./scripts/report.sh`.
3. **Stealth edit demo:** change a byte, restore mtime with `touch -r` → still caught by hash.
4. `integrityctl pause` / `resume`, `integrityctl add ~/other`, `kill -USR1 $(cat integrityd.pid)`.
5. Stop daemon, edit files, start → offline changes reported.
6. Edit one log line → `integrityd verify-log` flags it.
Fallback if inotify misbehaves in the VM: `--mode scan` with `scan_interval=2` (documented).

---

## 13. Coursework Alignment and Reading List `[ORIGINAL][ADDED]`
Original grading map: **Mini-Project: Full · Mid-Sem Eval: Core · Theory Exam: Concepts · Class Test: Partial.** Mapping to the real scheme (syllabus): Mini-project 20 marks = the full system S0–S10 and viva; Mid-sem practical (20) = S1–S3 (scanner, stat, hashing, snapshot I/O, shell basics); written theory (40) = the concepts in Section 2 (file system, inodes, processes, signals, stdio, shell filters); internal assessment 5 = submissions of milestone demos.

Original reading list (kept): (1) W. R. Stevens, *Advanced Programming in the UNIX Environment*, 3rd ed., Addison-Wesley, 2013 — file I/O, process control, signals. (2) B. W. Kernighan and R. Pike, *The Unix Programming Environment*, Prentice-Hall, 1984 — small composable tools. (3) N. Matthew and R. Stones, *Beginning Linux Programming*, 4th ed., Wiley, 2007 — directory and file handling. (4) C. Newham, *Learning the bash Shell*, 3rd ed., O'Reilly, 2005 — the shell reporting layer. Added: Linux man pages `inotify(7)`, `signal-safety(7)`, `poll(2)`, `fifo(7)`, `daemon(7)`; FIPS 180-4 (SHA-256); course texts Das, *Unix Concepts and Applications* and Kanetkar, *UNIX Shell Programming*.

---

## 14. Limitations and Future Scope `[ADDED]`
Limitations: Linux only (inotify); an attacker with root can alter daemon, snapshot and log together; ctime tell can be defeated by changing the system clock as root; hash chain is local (not anchored off-host); no rename tracking across roots; no extended attributes/ACLs; hashing large trees is I/O-bound. Future scope: chain-head anchoring to a remote host, `fanotify`, xattr/ACL tracking, a config-driven policy per path, a systemd unit file, signed snapshots.

---

## 15. Decision Register
| ID | Decision | Reason |
|---|---|---|
| D-01 | **C11 + gcc**, C standard library/POSIX/Linux headers, no third-party libs | Syllabus and original handbook are C; APIs are C; simplest semantics for fork/signals; helpers kept tiny (5.12) |
| D-02 | Core built from syllabus primitives; inotify/poll/fnmatch/getopt/SHA-256 as marked diversions; `scan` mode is syllabus-only | Owner: little diversion only |
| D-03 | Repo layout in 0.4 | Original modules kept + missing ones added |
| D-04 | Differentiator: readable inode-level baseline + real-time + tamper-evident log + live control + shell layer | Original lacked a problem statement |
| D-05 | Threat model in 1.3 | Original claimed "append-only" without enforcement |
| D-06 | Hybrid detection mode (inotify + periodic scan), `scan` mode as syllabus-only fallback | Real-time per original, robust against missed events |
| D-07 | Single-threaded `poll()` loop; ≤ 1 worker child | Simplicity; demonstrates fork/pipe without threads |
| D-08 | Debounce 200 ms / cap 1000 ms | Avoid repeated MODIFY events |
| D-09 | Self-pipe signal handling; SIGUSR1 = rescan | Async-signal safety |
| D-10 | Text formats with checksum trailer; atomic temp+fsync+rename | Debuggable, crash-safe |
| D-11 | ctime in the stat tuple | Cheap tamper tell; user space cannot set ctime |
| D-12 | Hash-chained log | Tamper evidence |
| D-13 | FIFO 0600 + per-client reply FIFO, request/reply protocol | Original had no reply channel and a 0666 FIFO |
| D-14 | SHA-256 via `open/read` | Original djb2/fgetc was non-cryptographic and slow |
| D-15 | Classification rules incl. MOVED, REPLACED, stealth flag | Original had only create/modify/delete/perms |
| D-16 | Baseline advances after reporting | Report each change once |
| D-17 | Recursive inotify with wd↔path maps, subtree walk on new dirs, overflow rescan | Original watched one directory |
| D-18 | Forked deep-verify worker with pipe | Responsiveness; shows fork/pipe/wait |
| D-19 | Alert hook via fork/execve, validated, rate-limited | Shows exec; safe |
| D-20 | Double-fork daemonisation, pid file, fd redirection | Original named "daemon" but had none |
| D-21 | Roots are snapshot state; config only seeds; RELOAD for the rest | Resolves ADD vs config conflict |
| D-22 | CLI subcommands incl. `check`, `baseline`, `hash`, `verify-log` | Testable without a daemon |
| D-23 | Small C helpers (Str, Vec, HashTab) only | Owner: avoid needless from-scratch work |
| D-24 | Error/edge-case table | Audit gap |
| D-25 | Milestones S0–S10; minimum S0–S7 | Original seven stages preserved |
| D-26 | Test plan T01–T24 | Audit gap |
Change log vs original handbook: all original capabilities, pipeline, syscall list, build stages, layout, reference code behaviour, verification list, alignment table and reading list are retained; replaced: djb2 hash, single-directory watcher, blocking FIFO loop, `signal()`, `0666` FIFO, stdout replies.

---

## 16. Preparing the Project Report (written later) `[ADDED]`
| Report chapter | Take from |
|---|---|
| Abstract, motivation | 1.1, 1.2 |
| Requirements and constraints | 0.2, 2, 1.3 |
| System design and architecture | 3, 4 |
| Implementation (modules and syscalls) | 5, 6, 8, original syscall table below |
| Testing | 10, 11 |
| Demonstration | 12 |
| Limitations and future scope | 14 |
| References | 13 |

**Original system-call table (kept):** directory traversal `opendir() readdir() closedir()`; metadata `stat() lstat() S_ISDIR()`; buffered I/O `fopen() fprintf() fclose()`; signals `signal() kill() getpid()`; change events `inotify_init() inotify_add_watch() read()`; control pipe `mkfifo() open() write()`. **Added:** `open/read/close/fstat`, `fork/execve/waitpid/_exit/setsid/umask/chdir/dup2/pipe2`, `sigaction/alarm`, `poll`, `rename/fsync`, `readlink/realpath`, `fnmatch`.

*End of integrityd specification.*
