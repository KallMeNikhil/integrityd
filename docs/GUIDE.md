# integrityd: Installation, Verification and Demo Guide

This guide takes you from a machine with nothing installed to a complete, evaluated run of
integrityd. It assumes only that the source code is already on the machine.

| Part | What it covers | Time |
|---|---|---|
| [1](#part-1-prepare-the-system) | Prepare the system | 10 min |
| [2](#part-2-get-the-code-ready-and-build) | Get the code ready and build | 5 min |
| [3](#part-3-run-the-automated-tests) | Run the automated tests | 5–10 min |
| [4](#part-4-quick-demo-one-command) | Quick demo (one command) | 2 min |
| [5](#part-5-full-manual-walkthrough) | Full manual walkthrough of every feature | 45 min |
| [6](#part-6-evaluation-checklist) | Evaluation checklist | reference |
| [7](#part-7-troubleshooting) | Troubleshooting | reference |
| [8](#part-8-where-each-concept-lives-in-the-code) | Where each concept lives in the code (viva) | reference |

**Short on time?** Do Parts 1–4, then the sections marked ★ in Part 5.

Other documents: `README.md` (reference for commands and every config key),
`docs/DESIGN.md` (the specification), `docs/DECISIONS.md` (choices made during the build).

---

## Part 1: Prepare the system

integrityd needs a **real Linux kernel**, because real-time detection uses inotify. Pick the
path that matches your machine.

| Your machine | Use |
|---|---|
| Ubuntu or Debian (bare metal or VM) | Go to [1.2](#12-install-the-packages) |
| Windows 10/11 | WSL2: start at [1.1](#11-windows-only-install-wsl2) |
| macOS | An Ubuntu VM (for example UTM or VirtualBox), then [1.2](#12-install-the-packages) |

WSL1 does **not** work. It was developed and tested on WSL2 with Ubuntu 26.04 and gcc 15.
Other distributions and compilers should work but have not been tested.

### 1.1 Windows only: install WSL2

Open **PowerShell as Administrator**:

```powershell
wsl --install -d Ubuntu
```

Restart if asked, then open **Ubuntu** from the Start menu and create a user name and
password when prompted. Back in PowerShell, confirm that it is version 2:

```powershell
wsl -l -v
```

The `VERSION` column must show `2`. If it shows `1`, run `wsl --set-version Ubuntu 2`.

Everything from here on runs **inside the Ubuntu terminal**, not PowerShell.

### 1.2 Install the packages

```bash
sudo apt-get update
sudo apt-get install -y build-essential gawk procps shellcheck
```

| Package | Needed for |
|---|---|
| `build-essential` | `gcc` and `make`, plus the sanitizer libraries used by `make asan` |
| `gawk` | test T03 (`strtonum`); stock Ubuntu may only have `mawk` |
| `procps` | `ps`, used by the tests and `launch.sh status` |
| `shellcheck` | optional: lints the shell scripts |

There are no other dependencies: no OpenSSL, no Python, no third-party libraries.

On other distributions, install the equivalents (Fedora:
`sudo dnf install gcc make gawk procps-ng ShellCheck`).

### 1.3 Check the system

```bash
gcc --version | head -1
make --version | head -1
uname -sr
ls /proc/sys/fs/inotify
cat /proc/sys/fs/inotify/max_user_watches
df -h /tmp
```

**Expect:** a gcc and a make version, `Linux ...`, and three inotify files:
`max_queued_events max_user_instances max_user_watches`. If `/proc/sys/fs/inotify` is missing,
you are not on a real Linux kernel.

`/tmp` needs at least 1 GB free for the tests. The worker demo in section 5.8 uses a 2 GB
sparse file, which takes almost no real disk space on normal Linux filesystems.

---

## Part 2: Get the code ready and build

### 2.1 Put the code on the Linux filesystem

Any location works, but this guide uses `~/integrityd`.

**From GitHub** (the repository is private until it is made public, so you need access and `gh auth login` or a token):

```bash
git clone https://github.com/KallMeNikhil/integrityd.git ~/integrityd
```

**From a copy on a Windows drive (WSL):**

```bash
cp -r /mnt/c/path/to/integrityd ~/integrityd
```

**From a zip or USB copy:** extract it so that `~/integrityd/Makefile` exists.

> **Why not run from `/mnt/c`?** Building there works, but Windows drives show every file as
> mode 0777 and can't hold FIFOs. A hook stored there would be rejected as world-writable.
> Keep the code, the run directory and the watched trees on the Linux filesystem.

### 2.2 Repair anything the copy may have damaged

Copies through Windows or zip tools can lose executable bits and turn line endings into
CRLF. These commands are safe to run even if nothing is wrong:

```bash
cd ~/integrityd
grep -rlI $'\r' Makefile scripts tests src | xargs -r sed -i 's/\r$//'
chmod +x scripts/* tests/run_integration.sh tests/integration/*.sh
```

### 2.3 Build

```bash
cd ~/integrityd
make clean
make
```

**Expect:** `gcc` lines with no warnings, ending with the link step that produces
`build/integrityd`. Confirm it:

```bash
build/integrityd version
```

**Expect:** `integrityd 1.0.0`

If your compiler rejects the code only because of a new warning (the build uses `-Werror`),
see [Part 7](#part-7-troubleshooting).

### 2.4 Set up your shell (do this in every terminal you open)

```bash
cd ~/integrityd
export REPO=$PWD
export D=$HOME/integrityd-demo
export R=$HOME/.integrityd
export CONF=$R/integrityd.conf
export PATH="$REPO/scripts:$REPO/build:$PATH"
```

`D` is the demo workspace and `R` is the default run directory. You can now type `integrityd`,
`integrityctl`, `launch.sh` and `report.sh` from anywhere.

To make this permanent, add those `export` lines to `~/.bashrc` with `REPO=$HOME/integrityd`.

---

## Part 3: Run the automated tests

Run these as a **normal user**, not root (some permission tests are skipped for root).

```bash
cd ~/integrityd
make test
```

**Expect:** 11 unit-test lines ending in `0 failed`, then 25 `PASS Tnn_...` lines, and last:

```
integration: 25 passed, 0 failed
```

Then the same suite under AddressSanitizer and UBSan:

```bash
make asan
```

**Expect:** the same final line. A memory error or leak in the daemon fails the test that hit it.

Optional lint (no output means clean):

```bash
shellcheck -S warning scripts/*.sh scripts/integrityctl tests/run_integration.sh
```

To run one test:

```bash
make integration T=T07
```

To see what a test checks, run it directly. Its `ok:` lines describe each check:

```bash
INTEGRITYD=$PWD/build/integrityd SCRIPTS=$PWD/scripts bash tests/integration/T07_stealth.sh
```

### What each test covers

| Test | Scenario |
|---|---|
| T01 | Baseline then check on an unchanged tree exits 0 |
| T02 | Symlink loop, dangling link and FIFO in the tree cause no hang |
| T03 | Weird file names survive the snapshot, the log and the control protocol |
| T04 | `integrityd hash` equals `sha256sum`, from 0 bytes to 100 MB |
| T05 | New file gives `CREATED` |
| T06 | Append gives `MODIFIED` with new size and hash |
| T07 | Stealth edit gives `MODIFIED stealth=1` |
| T08 | Delete gives `DELETED` |
| T09 | `chmod` gives `PERMS`, no `MODIFIED` |
| T10 | Change three directory levels down |
| T11 | New directory populated immediately: nothing missed |
| T12 | Rename gives `MOVED`; delete + create is not `MOVED` |
| T13 | Editor atomic save gives one `MODIFIED` |
| T14 | `status`, `ping`, `list`; protocol errors; flood of bad lines |
| T15 | Pause, change, resume: changes reported with `during_pause=1` |
| T16 | Add a root live; remove it and it goes silent |
| T17 | `kill -USR1` rescan in scan mode; `SIGUSR2`; `SIGHUP`/`reload` |
| T18 | Stop, change, start: offline changes reported; `launch.sh`; daemon really detached |
| T19 | Log tampering: `verify-log` names the record; torn last line handled |
| T20 | Rotation keeps sequence and chain continuous |
| T21 | Corrupt snapshot renamed, re-baselined, `ERROR` logged |
| T22 | Worker deep verify; killing it leaves the daemon alive |
| T23 | Alert hook arguments, severity filter, rate limit, rejected hooks |
| T24 | `report.sh` counts match a scripted log in all formats |
| T25 | Extra edge cases: root deleted and re-created, unreadable paths, live FIFO, file ↔ directory, excludes, file root |

---

## Part 4: Quick demo (one command)

`scripts/demo.sh` runs the core story unattended in a temporary directory and removes it
afterwards. Nothing else on the system is touched.

Run it straight through:

```bash
scripts/demo.sh
```

Or wait for Enter between steps, which is best for presenting:

```bash
scripts/demo.sh --pause
```

It shows: starting the daemon, create/edit/delete/chmod, the stealth edit, pause/resume, adding
a root, `SIGUSR1`, offline changes across a restart, log tampering (`BROKEN 2`, exit code 4)
and the reports.

---

## Part 5: Full manual walkthrough

This part exercises every feature by hand, in order. Each section builds on the previous
one, so run them in sequence.

### 5.0 Layout: two terminals

Run the [2.4 setup block](#24-set-up-your-shell-do-this-in-every-terminal-you-open) in both.

**Start from a clean state** (Terminal A). This deletes only the demo workspace and the
default run directory, so skip it if you have integrityd data you want to keep:

```bash
launch.sh stop
rm -rf "$R" "$D"
```

**Create the demo tree** (Terminal A):

```bash
mkdir -p "$D/site/etc" "$D/site/www/assets" "$D/other"
echo "hello"      > "$D/site/www/index.html"
echo "obsolete"   > "$D/site/www/old.html"
echo "logo"       > "$D/site/www/assets/logo.txt"
echo "token=42"   > "$D/site/etc/secret.conf"
printf 'listen 80\n' > "$D/site/etc/server.conf"
ln -s etc/server.conf "$D/site/current.conf"
echo "other data" > "$D/other/data.txt"
```

**Live log view** (Terminal B). This shows the log in columns with hashes shortened, and keeps
following it across rotation. It complains that the file doesn't exist until the daemon
starts, which is harmless:

```bash
tail -F "$R/change.log" | awk -F'\t' '{ d=$6; gsub(/[0-9a-f]{56}/,"..",d); printf "%-4s %-5s %-13s %s  %s\n",$1,$3,$4,$5,d; fflush() }'
```

Leave it running. "Watch Terminal B" below means this view. Commands go in Terminal A.

### 5.1 Offline tools (no daemon)

These commands use a separate run directory so they don't affect the live demo.

**Hashing.** The output must match `sha256sum` exactly:

```bash
integrityd hash "$D/site/etc/server.conf"
sha256sum "$D/site/etc/server.conf"
```

**Scanner.** Prints what the directory walk sees. The columns below are type, path, mode and
size. Types are `f` file, `d` directory, `l` symlink:

```bash
integrityd check --scan-only --rundir "$D/off" --watch "$D/site" | cut -f1,2,5,9
```

**Baseline.** Creates the snapshot without any alerts:

```bash
integrityd baseline --rundir "$D/off" --watch "$D/site"
```

**Expect:** `baseline: 10 records in 1 roots -> .../off/snapshot.db`

Look at the snapshot format, including the `#END` line with its record count and checksum:

```bash
cut -c1-90 "$D/off/snapshot.db"
```

**Check an unchanged tree:**

```bash
integrityd check --rundir "$D/off"; echo "exit=$?"
integrityd check --rundir "$D/off" --deep; echo "exit=$?"
```

**Expect:** no events and `exit=0` both times. `--deep` hashes every file.

**Check after changes:**

```bash
echo "offline edit" >> "$D/site/www/index.html"
echo "temporary"    >  "$D/site/www/offline.txt"
integrityd check --rundir "$D/off"; echo "exit=$?"
```

**Expect:** `CREATED` and `MODIFIED` lines in log format, and `exit=1`.
Plain `check` changes nothing, so running it again reports the same two events.

**Adopt the changes:**

```bash
integrityd check --rundir "$D/off" --update > /dev/null
integrityd check --rundir "$D/off"; echo "exit=$?"
integrityd verify-log --rundir "$D/off"
```

**Expect:** `exit=0`, then `OK <n> head=<64 hex digits>`. The events went into `$D/off/change.log`.

Tidy up before the live part:

```bash
rm "$D/site/www/offline.txt"
```

### 5.2 ★ Configure and start the daemon

Create a config. `scan_interval=3600` is deliberate: with no periodic scan during the demo,
everything you see in sections 5.3 and 5.4 is proved to come from inotify in real time.
The default is 60.

```bash
mkdir -p -m 700 "$R"
cat > "$CONF" <<'EOF'
mode=hybrid
scan_interval=3600
deep_every=2
exclude=*.tmp
exclude=cache/
log_max_bytes=10485760
log_keep=5
EOF
```

Start it. The first start takes the baseline silently:

```bash
launch.sh start --watch "$D/site"
```

**Expect:** `started pid <N>`. Terminal B shows `DAEMON_START ... baseline=initial;records=10;...`

Confirm it is a real daemon:

```bash
launch.sh status
ps -o pid,ppid,sid,tty,comm -p "$(cat "$R/integrityd.pid")"
ls -la "$R"
```

**Expect:**

- `status`: `running pid N`, then a line with `state=running mode=hybrid roots=1 ...`
- `ps`: `TT` is `?` (no controlling terminal), and `SID` differs from `PID` (the double fork: it
  isn't a session leader)
- `ls`: `change.log`, `snapshot.db`, `control.fifo` (a `p` file, mode 0600), `integrityd.pid`,
  `daemon.out` and `reply/`, with the directory itself at mode 0700

### 5.3 ★ Real-time detection

Run each block, then watch Terminal B. Events appear after the 200 ms debounce window.

**Create:**

```bash
echo "new page" > "$D/site/www/new.html"
```

**Expect:** `WARN CREATED .../www/new.html  type=f;size=9;sha=..;mode=0644`

**Modify:**

```bash
echo "<p>more</p>" >> "$D/site/www/index.html"
```

**Expect:** `ALERT MODIFIED` with `old_sha`, `new_sha`, `old_size` and `new_size`.

**Delete:**

```bash
rm "$D/site/www/old.html"
```

**Expect:** `ALERT DELETED ... type=f;old_sha=..;old_size=9`

**Permission-only change** (reported separately from content):

```bash
chmod 600 "$D/site/etc/secret.conf"
```

**Expect:** `ALERT PERMS ... old_mode=0644;new_mode=0600;...` and **no** `MODIFIED`.

**Nested directories, created and populated immediately:**

```bash
mkdir -p "$D/site/www/a/b/c" && echo deep > "$D/site/www/a/b/c/deep.txt"
```

**Expect:** `CREATED` for `a`, `a/b`, `a/b/c` and `deep.txt`. Now prove the new tree is watched:

```bash
echo deeper >> "$D/site/www/a/b/c/deep.txt"
```

**Expect:** `MODIFIED .../a/b/c/deep.txt`

**Rename:**

```bash
mv "$D/site/www/new.html" "$D/site/www/renamed.html"
```

**Expect:** one `WARN MOVED .../renamed.html  from=.../new.html;...`, not a delete plus a create.

**Editor-style atomic save** (write a temp file, rename it over the original):

```bash
echo "version 2" > "$D/site/www/.index.html.tmp"
mv "$D/site/www/.index.html.tmp" "$D/site/www/index.html"
```

**Expect:** exactly one `MODIFIED .../index.html`. The temp file never appears.

**Same content, new inode:**

```bash
cp "$D/site/etc/server.conf" "$D/site/etc/.s.tmp" && mv "$D/site/etc/.s.tmp" "$D/site/etc/server.conf"
```

**Expect:** `INFO REPLACED ... old_ino=..;new_ino=..`

**Burst of writes (debounce):**

```bash
for i in $(seq 1 20); do echo "$i" >> "$D/site/www/index.html"; done
```

**Expect:** one `MODIFIED`, not twenty.

**Type change:**

```bash
rm "$D/site/www/assets/logo.txt" && mkdir "$D/site/www/assets/logo.txt"
```

**Expect:** `ALERT TYPE_CHANGED ... old_type=f;new_type=d`

**Symlink retarget:**

```bash
ln -sfn etc/secret.conf "$D/site/current.conf"
```

**Expect:** `MODIFIED .../current.conf` with `old_target` and `new_target`.

**Excluded paths** (`*.tmp` and `cache/` from the config, plus the built-in `*.swp *.swx *~ .#*`):

```bash
echo x > "$D/site/www/debug.tmp"
mkdir "$D/site/www/cache" && echo y > "$D/site/www/cache/c.txt"
```

**Expect:** nothing in Terminal B.

**FIFO in the tree** (opening one would hang a naive program forever):

```bash
mkfifo "$D/site/www/pipe"
integrityctl ping
```

**Expect:** `CREATED ... type=o`, and `OK pong`: the daemon recorded it without opening it.

**Awkward file names:**

```bash
echo a > "$D/site/www/with space"
echo b > "$D/site/www/semi;colon=x"
echo c > "$D/site/www/"$'new\nline'
```

**Expect:** three `CREATED` lines, each on one line, with the newline shown as `%0A`.

**Unreadable file** (skip if you are root):

```bash
chmod 000 "$D/site/www/renamed.html"
```

**Expect:** `PERMS` and `WARN UNREADABLE ... errno=13`, logged once.

```bash
chmod 644 "$D/site/www/renamed.html"
```

**Expect:** `PERMS` only. The content didn't change, so there is no `MODIFIED`.

### 5.4 ★ Stealth edit (the key demonstration)

Change a file's content while keeping everything `ls -l` shows identical:

```bash
F=$D/site/etc/server.conf
ls -l --time-style=full-iso "$F"
touch -r "$F" "$D/ref"
printf 'listen 81\n' > "$F"
touch -r "$D/ref" "$F"
ls -l --time-style=full-iso "$F"
```

**Expect:** the two `ls -l` lines are identical, yet Terminal B shows:

`ALERT MODIFIED .../etc/server.conf  old_sha=..;new_sha=..;old_size=10;new_size=10;stealth=1`

The SHA-256 hash caught what size and timestamp hide. A process can restore mtime but not ctime,
which is why a metadata scan notices it too.

### 5.5 ★ Live control pipe

```bash
integrityctl ping
integrityctl version
integrityctl status
integrityctl list
integrityctl tail 5
```

**Protocol errors.** Exit code 2 and a clear message, and the daemon keeps running:

```bash
integrityctl raw FROBNICATE; echo "exit=$?"
integrityctl tail 999; echo "exit=$?"
integrityctl add "$D/site/www"; echo "exit=$?"
```

**Expect:** `ERR 400 unknown command`, `ERR 422 TAIL n: n must be 1..200`, and
`ERR 422 already covered by root ...`, each with `exit=2`.

**Pause and resume: a pause never creates a blind spot.**

```bash
integrityctl pause
echo "edited while paused" >> "$D/site/www/renamed.html"
echo "new while paused"    >  "$D/site/www/paused.txt"
sleep 2
integrityctl status | tr ' ' '\n' | grep state
```

**Expect:** `state=paused`, and Terminal B shows only `PAUSED`.

```bash
integrityctl resume
```

**Expect:** `OK resumed changes=...`, and Terminal B shows `RESUMED`, then both changes tagged
`during_pause=1`.

**Add a root live.** The baseline is silent, then changes are reported:

```bash
integrityctl add "$D/other"
integrityctl list
echo "changed" >> "$D/other/data.txt"
```

**Expect:** `OK added merged=0`, two roots, `WATCH_ADDED`, then `MODIFIED .../other/data.txt`.

**Remove it:**

```bash
integrityctl remove "$D/other"
echo "ignored" >> "$D/other/data.txt"
```

**Expect:** `OK removed records=2`, `WATCH_REMOVED`, and no event for the second command.

**On-demand scans:**

```bash
integrityctl rescan
integrityctl rescan "$D/site/etc"
integrityctl deepverify
```

**Expect:** `OK files=N changes=0 ms=N` each time, with `SCAN_END` lines in Terminal B.

**Rebaseline: accept the current state without alerts.**

```bash
integrityctl pause
echo "approved change" >> "$D/site/www/renamed.html"
integrityctl rebaseline
integrityctl resume
```

**Expect:** `OK added=0;removed=0;changed=1;records=N`, a `REBASELINE` line, then
`OK resumed changes=0`: the change was accepted, so it is never alerted.

### 5.6 ★ Signals

```bash
PID=$(cat "$R/integrityd.pid")
kill -USR1 "$PID"
kill -USR2 "$PID"
integrityctl kill-rescan
```

**Expect:** `SCAN_END kind=signal` (for both `kill -USR1` and `kill-rescan`), and an INFO `STATUS`
line for `SIGUSR2`. `SIGHUP` is shown in the next section; `SIGTERM` (what `launch.sh stop` sends) in 5.10.

### 5.7 Detection modes and config reload

Reload changes behaviour without a restart. Switch to timer-only `scan` mode:

```bash
sed -i 's/^mode=.*/mode=scan/; s/^scan_interval=.*/scan_interval=5/' "$CONF"
kill -HUP "$(cat "$R/integrityd.pid")"
sleep 1
integrityctl status
```

**Expect:** `CONFIG_RELOAD mode=scan;scan_interval=5;...`, and `mode=scan` and `watches=0` in the
status: inotify is off.

```bash
echo "scan mode edit" >> "$D/site/www/renamed.html"
```

**Expect:** a delay of up to 5 seconds, then `MODIFIED`. `SCAN_END` lines now arrive every 5
seconds, alternating `kind=meta` and `kind=deep` because `deep_every=2`.

This is also the fallback if inotify misbehaves on a VM. Switch back:

```bash
sed -i 's/^mode=.*/mode=hybrid/; s/^scan_interval=.*/scan_interval=3600/' "$CONF"
integrityctl reload
integrityctl status
```

**Expect:** `OK reloaded mode=hybrid`, and `watches` above 0 again.

A bad config is refused and the old one stays in force:

```bash
echo "nonsense line" >> "$CONF"
integrityctl reload; echo "exit=$?"
sed -i '/^nonsense line$/d' "$CONF"
integrityctl reload
```

**Expect:** `ERR 422 config error: ...` with `exit=2`, then `OK reloaded mode=hybrid`.

> If you start the daemon with `--mode` on the command line, that value keeps overriding the
> config until the next start. That's why this walkthrough doesn't use it.

### 5.8 Deep-verify worker (fork, pipe, wait)

Normally the worker runs only for trees above 2000 records. Lower the limit, and add a large
sparse file so a pass lasts long enough to observe:

```bash
echo "worker_min_records=0" >> "$CONF"
integrityctl reload
truncate -s 2G "$D/site/big.bin"
until grep -q big.bin "$R/change.log"; do sleep 1; done
```

The `until` loop waits while the daemon hashes the new file, which takes a few seconds.

**Run a deep pass in the worker.** The main loop must stay responsive:

```bash
integrityctl deepverify
integrityctl status | tr ' ' '\n' | grep worker
integrityctl ping
ps -o pid,ppid,comm --ppid "$(cat "$R/integrityd.pid")"
```

**Expect:** `OK worker started pid=N`, `worker=running`, an immediate `OK pong`, and a child
`integrityd` process. Wait about 10 seconds:

**Expect in Terminal B:** `SCAN_END kind=deep;worker=1;files=N;changes=0;ms=N`

**Kill the worker mid-run:**

```bash
out=$(integrityctl deepverify); echo "$out"
sleep 1
kill -9 "${out#*pid=}"
sleep 1
integrityctl status | tr ' ' '\n' | grep worker
integrityctl ping
```

**Expect:** Terminal B shows `WARN ERROR  what=worker;pid=N;signal=9;done=0`. The daemon is still
alive: `worker=idle` and `OK pong`.

Clean up:

```bash
rm "$D/site/big.bin"
sed -i '/^worker_min_records/d' "$CONF"
integrityctl reload
```

### 5.9 Alert hook (fork, execve)

The hook script must be on the Linux filesystem, because a world-writable hook is rejected.

```bash
cat > "$D/hook.sh" <<EOF
#!/bin/bash
echo "event=\$1 path=\$2 severity=\$3 seq=\$4 detail=\$INTEGRITYD_DETAIL" >> "$D/hook.out"
EOF
chmod 700 "$D/hook.sh"
printf 'alert_hook=%s\nhook_min_severity=WARN\nhook_rate_per_sec=10\n' "$D/hook.sh" >> "$CONF"
integrityctl reload
echo "trigger" >> "$D/site/www/renamed.html"
sleep 1
cat "$D/hook.out"
```

**Expect:** a line starting `event=MODIFIED path=.../renamed.html severity=ALERT seq=N detail=old_sha=...`

**A file name can't inject commands**, because the hook is run with `execve`, never a shell:

```bash
echo x > "$D/site/www/a; touch pwned"
sleep 1
tail -n 1 "$D/hook.out"
```

**Expect:** `event=CREATED path=.../www/a; touch pwned severity=WARN ...`: the whole name arrived
as one argument, and no command was run.

**An unsafe hook is rejected:**

```bash
chmod 777 "$D/hook.sh"
integrityctl reload
```

**Expect:** Terminal B shows `ERROR ... what=hook_rejected;...why=group- or world-writable`. The daemon
keeps running without the hook. Restore it and remove it from the config:

```bash
chmod 700 "$D/hook.sh"
sed -i '/^alert_hook=/d; /^hook_min_severity=/d; /^hook_rate_per_sec=/d' "$CONF"
integrityctl reload
```

### 5.10 ★ Restart persistence and offline changes

**Stop gracefully:**

```bash
launch.sh stop
ls "$R"
```

**Expect:** `stopped pid N`, `DAEMON_STOP` in Terminal B, and no `integrityd.pid` or `control.fifo`
any more. `snapshot.db` and `change.log` remain.

**Change files while nothing is running,** including a stealth edit:

```bash
echo "offline edit" >> "$D/site/www/index.html"
rm "$D/site/www/paused.txt"
echo "offline new" > "$D/site/www/offline-new.txt"
chmod 640 "$D/site/etc/server.conf"
F=$D/site/etc/secret.conf
touch -r "$F" "$D/ref"; printf 'token=99\n' > "$F"; touch -r "$D/ref" "$F"
```

**Start again.** No `--watch` is needed, because the roots are stored in the snapshot:

```bash
launch.sh start
```

**Expect:** `DAEMON_START baseline=loaded`, then `MODIFIED`, `DELETED`, `CREATED` and `PERMS`, all
tagged `offline=1`, `MODIFIED .../secret.conf` with `stealth=1;offline=1`, and finally
`SCAN_END kind=startup_deep`.

**Only one daemon per run directory:**

```bash
integrityd daemon --foreground; echo "exit=$?"
integrityd baseline --watch "$D/site"; echo "exit=$?"
launch.sh start; echo "exit=$?"
```

**Expect:** `exit=3` all three times, with messages such as `already running (pid N, ...)`.

### 5.11 Log rotation

```bash
sed -i 's/^log_max_bytes=.*/log_max_bytes=2000/; s/^log_keep=.*/log_keep=3/' "$CONF"
integrityctl reload
for i in $(seq 1 30); do echo "$i" > "$D/site/www/rot_$i.txt"; done
sleep 2
ls -l "$R"/change.log*
integrityd verify-log
```

**Expect:** `change.log`, `change.log.1` and more, never beyond `.3`. Terminal B shows
`LOG_ROTATE` lines. `verify-log` prints `OK ...`, adding `anchor=seq N` if the oldest file was
rotated away: the chain is then checked from the oldest record still present.

Check the handover between two files. The sequence continues and the new file starts with
`cont=1`:

```bash
tail -n 1 "$R/change.log.1" | cut -f1,4,6
head -n 1 "$R/change.log"   | cut -f1,4,6
```

Restore normal settings:

```bash
sed -i 's/^log_max_bytes=.*/log_max_bytes=10485760/; s/^log_keep=.*/log_keep=5/' "$CONF"
integrityctl reload
```

### 5.12 ★ Reports

```bash
report.sh
report.sh --since "$(date -u +%F)"
report.sh --format text
report.sh --format text --events
report.sh --format html
```

**Expect:**

- One line: `Report: N modified, N created, N deleted, N permission changes, N moved`
- With `--since`: the same, plus `(since <date>)`. Dates are UTC and accept `YYYY-MM-DD` or
  `"YYYY-MM-DD HH:MM"`, and `--until` works the same way
- `text`: a count per event type, recent alerts in plain language, and the most-changed paths.
  The stealth edit reads `size and mtime unchanged: stealth edit`
- `--events`: adds INFO events such as `SCAN_END`
- `html`: `wrote .../report.html`, a static page with no JavaScript

Open the HTML report on a Linux desktop:

```bash
xdg-open "$R/report.html"
```

Or from WSL:

```bash
explorer.exe "$(wslpath -w "$R/report.html")"
```

### 5.13 ★ Log tampering

Stop the daemon and keep a clean copy, so the log can be restored between attacks. Terminal B
may reprint the log during this section, because these commands replace the file. Ignore that.

```bash
launch.sh stop
integrityd verify-log
cp "$R/change.log" "$D/clean.log"
```

**Attack 1, edit a record** (this changes one letter in the timestamp of the second line):

```bash
sed -n 2p "$R/change.log" | cut -f1
sed -i '2s/T/t/' "$R/change.log"
integrityd verify-log; echo "exit=$?"
```

**Expect:** `BROKEN <seq> chain mismatch at .../change.log:2`, where `<seq>` is the number printed
by the first command, and `exit=4`. One changed character is enough.

**Attack 2, delete a record:**

```bash
cp "$D/clean.log" "$R/change.log"
sed -i '2d' "$R/change.log"
integrityd verify-log; echo "exit=$?"
```

**Expect:** `BROKEN <seq> sequence gap (found ...)`, `exit=4`.

**Attack 3, a torn last line** (as left by a crash during a write):

```bash
head -c -20 "$D/clean.log" > "$R/change.log"
integrityd verify-log; echo "exit=$?"
```

**Expect:** `TRUNCATED <seq> partial line at ...`, `exit=4`.

The next writer repairs a torn tail and records that it did:

```bash
launch.sh start
sleep 1
launch.sh stop
integrityd verify-log; echo "exit=$?"
```

**Expect:** Terminal B shows `ERROR ... what=log_partial_line`, and `verify-log` is `OK` with `exit=0`.

> **Limit to state honestly:** the chain has no secret key. An attacker who rewrites every record
> from the edit onward produces a valid chain, and removing whole records from the very end
> can't be detected either. Copying `head=` to another location closes both gaps. The threat
> model is in `docs/DESIGN.md` section 1.3.

### 5.14 Corrupt snapshot

The daemon is stopped at this point. Change one digit inside a record:

```bash
sed -i '0,/\t0644\t/s//\t0600\t/' "$R/snapshot.db"
integrityd check; echo "exit=$?"
```

**Expect:** a message ending `checksum mismatch`, and `exit=4`.

Start the daemon. The list of roots was stored in the snapshot, so pass `--watch` again:

```bash
launch.sh start --watch "$D/site"
sleep 1
ls "$R"/snapshot.db*
integrityctl status
```

**Expect:** Terminal B shows `ERROR ... what=snapshot_corrupt`, then `DAEMON_START baseline=initial`.
The bad file is kept as `snapshot.db.corrupt.<timestamp>`, a fresh `snapshot.db` is in place, and
the status shows `roots=1`.

### 5.15 Clean up

```bash
launch.sh stop
rm -rf "$R" "$D"
```

Press `Ctrl+C` in Terminal B.

### 5.16 Optional: failure modes that need root

These can't run in the automated suite, because they change system limits. Note the current
value first and restore it afterwards. A new limit only applies to a daemon started after the change.

**inotify queue overflow.** The daemon is frozen with `SIGSTOP` during the burst, because otherwise
it empties the queue faster than a shell loop can fill it:

```bash
cat /proc/sys/fs/inotify/max_queued_events
sudo sysctl fs.inotify.max_queued_events=16
mkdir -p "$D/ov"
launch.sh start --watch "$D/ov"
kill -STOP "$(cat "$R/integrityd.pid")"
for i in $(seq 500); do touch "$D/ov/f$i"; done
kill -CONT "$(cat "$R/integrityd.pid")"
sleep 3
cut -f4 "$R/change.log" | sort | uniq -c
launch.sh stop
sudo sysctl fs.inotify.max_queued_events=<original value>
```

**Expect:** `OVERFLOW_RESCAN` in the counts, and all 500 files `CREATED`: the rescan found what
the overflowed queue lost.

**Watch limit** (`max_user_watches` counts every inotify user on the system, so desktop tools may be affected while it is lowered):

```bash
cat /proc/sys/fs/inotify/max_user_watches
sudo sysctl fs.inotify.max_user_watches=10
mkdir -p "$D/wl" && for i in $(seq 1 30); do mkdir "$D/wl/d$i"; done
launch.sh start --watch "$D/wl"
integrityctl tail 3
launch.sh stop
sudo sysctl fs.inotify.max_user_watches=<original value>
```

**Expect:** `ERROR what=watch_limit;...fallback=scan`. That root is then covered by periodic scans.

---

## Part 6: Evaluation checklist

### Original verification list (spec section 10)

| # | Requirement | Automated | Manual |
|---|---|---|---|
| 1 | Creation is detected | T05 | 5.3 |
| 2 | Content edits are caught, even when disguised | T07 | 5.4 |
| 3 | Deletion is detected | T08 | 5.3 |
| 4 | Permission-only changes are flagged separately | T09 | 5.3 |
| 5 | Nested directories are covered | T10, T11 | 5.3 |
| 6 | The control pipe responds live | T14–T16 | 5.5 |
| + | Signal-triggered rescan | T17 | 5.6 |
| + | Pause/resume without a blind spot | T15 | 5.5 |
| + | Restart persistence and offline changes | T18 | 5.10 |
| + | Log durability and tamper detection | T19, T20 | 5.11, 5.13 |

### Sign-off sheet

| Done | Item | Pass condition |
|---|---|---|
| ☐ | Build | No warnings; `integrityd version` prints `integrityd 1.0.0` |
| ☐ | `make test` | `integration: 25 passed, 0 failed` |
| ☐ | `make asan` | `integration: 25 passed, 0 failed` |
| ☐ | Quick demo | `scripts/demo.sh` finishes; step 6 shows `BROKEN 2` |
| ☐ | Offline tools (5.1) | Exit codes 0, 1, 0 as described |
| ☐ | Daemon detached (5.2) | No tty, `SID` differs from `PID` |
| ☐ | Real-time events (5.3) | Every listed event appears, within about a second |
| ☐ | Stealth edit (5.4) | Identical `ls -l`, `stealth=1` logged |
| ☐ | Control pipe (5.5) | All commands answer; pause leaves no gap |
| ☐ | Signals (5.6) | `SCAN_END kind=signal` and `STATUS` logged |
| ☐ | Modes and reload (5.7) | `watches=0` in scan mode; bad config refused |
| ☐ | Worker (5.8) | Responsive during a pass; survives `kill -9` of the worker |
| ☐ | Hook (5.9) | Correct arguments; no injection; unsafe hook rejected |
| ☐ | Restart (5.10) | Offline changes tagged `offline=1`; second daemon exits 3 |
| ☐ | Rotation (5.11) | Sequence continues; `verify-log` OK |
| ☐ | Reports (5.12) | All formats produced |
| ☐ | Tampering (5.13) | `BROKEN`, `TRUNCATED`, exit 4 |
| ☐ | Corrupt snapshot (5.14) | Renamed aside; fresh baseline; `ERROR` logged |

### Suggested 15-minute presentation

1. Show `make test` output, or run it beforehand and show the final line (Part 3).
2. Start the daemon and show `ps` (5.2).
3. Create, modify, delete, `chmod`, `mv` (5.3), with the log visible beside you.
4. The stealth edit (5.4).
5. Pause, change, resume, then `kill -USR1` (5.5, 5.6).
6. Stop, edit, start (5.10).
7. `report.sh --format text` (5.12).
8. Tamper with the log (5.13).

---

## Part 7: Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `bad interpreter: /bin/bash^M` or `$'\r': command not found` | CRLF line endings from a Windows copy | Run the first command in [2.2](#22-repair-anything-the-copy-may-have-damaged) |
| `Permission denied` running a script | Executable bits lost | Run the `chmod` command in 2.2 |
| `Makefile: *** missing separator` | CRLF in the Makefile, or tabs lost in the copy | Run 2.2; if it persists, get a fresh copy |
| Build stops with `error: ... [-Werror=...]` | A different gcc adds a new warning | Build without `-Werror`: `make CFLAGS="-std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra"`. Report the warning |
| `integrityd: command not found` | Shell not set up | Run the block in [2.4](#24-set-up-your-shell-do-this-in-every-terminal-you-open) |
| T03 fails with an awk error about `strtonum` | `awk` is `mawk` | `sudo apt-get install -y gawk` |
| `make asan` fails to link | Sanitizer libraries missing | Ubuntu: reinstall `build-essential`. Other distributions: install `libasan` and `libubsan` |
| A test fails only on a heavily loaded machine | Tests use real timing | Rerun it alone: `make integration T=T11` |
| `integrityctl: no reply (daemon not running?)` | Daemon stopped, a different run dir, or busy hashing a very large file inline | `launch.sh status`; `cat ~/.integrityd/daemon.out`; check `--rundir`. Retry after large-file hashing |
| `mkfifo` errors, or `what=control_fifo` in the log | Run dir on a Windows drive (`/mnt/c`) | Use a run dir on the Linux filesystem |
| Hook rejected as `group- or world-writable` | Hook on `/mnt/c`, or wrong mode | Move it to the Linux filesystem; `chmod 700` |
| Changes made from Windows aren't seen live | inotify on `/mnt/c` doesn't reliably see changes made by Windows programs | Watch a Linux path, or use `mode=scan` |
| `already running` after a crash | Live process still present | `launch.sh status`. A stale pid file is removed automatically |
| `ERROR what=watch_limit` | Too many directories for inotify | Raise it: `sudo sysctl fs.inotify.max_user_watches=524288`. The root keeps working through scans meanwhile |
| Nothing is logged for a file | It matches an exclude (`*.swp *.swx *~ .#*` or your config) | Check `$CONF` |
| `integrityd check` uses the wrong data | `integrityd` itself ignores `INTEGRITYD_RUNDIR` | Pass `--rundir`. Only the shell scripts read that variable |
| `tail -f` stops after rotation | `tail -f` follows the old file | Use `tail -F` |

**Debugging:** run in the foreground to see diagnostics in the terminal, and stop with `Ctrl+C`,
which is a normal graceful shutdown:

```bash
integrityd daemon --foreground --watch "$D/site"
```

**Running as root:** the same commands work, and the default run directory becomes
`/var/lib/integrityd`. Permission-denied scenarios won't trigger, because root can read everything.

---

## Part 8: Where each concept lives in the code

| Syllabus topic | Where | What to show |
|---|---|---|
| Directories, `opendir/readdir/closedir` | `src/scanner.c` | `walk()` |
| Inodes, `lstat`, `S_ISDIR` and friends | `src/scanner.c` | `record_set_stat()` |
| Low-level I/O, `open/read/fstat` | `src/hashing.c` | `hash_file()`, including the stability retry |
| Standard I/O, `fopen/fprintf/fflush` | `src/snapshot.c`, `src/logger.c` | `snapshot_save()`, `logger_write()` |
| Atomic replace, `rename`, `fsync` | `src/snapshot.c` | `snapshot_save()` |
| Signals, `sigaction`, self-pipe | `src/signals.c` | `on_signal()` |
| `alarm` as a timer | `src/daemon.c` | `on_tick()` |
| `fork`, `setsid`, `dup2`, redirection | `src/daemonize.c` | `daemonize()` |
| `fork`, `pipe`, `waitpid` | `src/worker.c`, `src/daemon.c` | `worker_start()`, `reap_children()` |
| `fork`, `execve` | `src/hook.c` | `hook_fire()` |
| FIFOs, `mkfifo` | `src/control.c`, `scripts/integrityctl` | `control_open()`, `control_send()` |
| `poll` event loop (diversion) | `src/daemon.c` | `event_loop()` |
| inotify (diversion) | `src/watcher.c` | `walk_add()`, `watcher_read()` |
| Classification rules | `src/compare.c` | `classify()`, `pair_moves()` |
| Hash chain | `src/logger.c` | `chain_next()`, `verify_log()` |
| SHA-256 (FIPS 180-4) | `src/sha256.c` | `compress()` |
| `grep/sed/awk`, pipes | `scripts/report.sh` | `records()`, `recent_alerts()` |
| Loops, functions, `case`, `trap` | `scripts/launch.sh`, `scripts/integrityctl` | `start()`, `stop()`, `escape()` |

### Questions an examiner is likely to ask

**Why a self-pipe instead of doing the work in the signal handler?** Only async-signal-safe functions
may run in a handler. `printf`, `malloc` and the hash table are not safe, so the handler writes
one byte and the `poll()` loop does the work.

**Why a double fork?** The first fork plus `setsid` detaches from the terminal. The second means
the daemon isn't a session leader, so it can never acquire a controlling terminal again.

**Why does the control FIFO have a second, write-only descriptor?** Without a writer, the read end
reports EOF whenever the last client closes, and `poll()` would spin.

**How can a metadata scan catch a stealth edit?** User space can restore mtime but not ctime, so the
stat tuple still differs and triggers a hash. Only a change of the system clock by root gets
around that, and the periodic deep hash covers that case.

**Why not open every file during a scan?** Opening a FIFO blocks forever and opening a device can
have side effects. Only regular files are opened, with `O_NOFOLLOW|O_NONBLOCK`, and `fstat`
confirms the type before anything is read.

**What is a hash chain, and what can't it do?** Each record's hash covers the previous hash, so one
edit breaks every later link. It has no secret key, so see the limit noted in 5.13.

**Why is a new directory watched before it is listed?** Files created during the listing would
otherwise be missed. Watch first and each new file is either seen by the listing or reported by the
watch, which closes the race.
