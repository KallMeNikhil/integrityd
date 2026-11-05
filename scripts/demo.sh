#!/bin/bash
set -u
here=$(cd "$(dirname "$0")" && pwd)
export INTEGRITYD=${INTEGRITYD:-$here/../build/integrityd}
[ -x "$INTEGRITYD" ] || { echo "build first: make" >&2; exit 1; }
PAUSE=0
[ "${1:-}" = "--pause" ] && PAUSE=1

DEMO=$(mktemp -d /tmp/integrityd-demo.XXXXXX)
RUN=$DEMO/run
TREE=$DEMO/demo
OTHER=$DEMO/other
L() { "$here/launch.sh" --rundir "$RUN" "$@"; }
C() { "$here/integrityctl" --rundir "$RUN" "$@"; }
R() { "$here/report.sh" --rundir "$RUN" "$@"; }
cleanup() { L stop >/dev/null 2>&1; rm -rf "$DEMO"; }
trap cleanup EXIT INT TERM

step() {
    printf '\n\033[1m== %s\033[0m\n' "$*"
    if [ $PAUSE -eq 1 ]; then read -r -p "(Enter) " _; fi
}
show() { printf '$ %s\n' "$*"; eval "$*"; }
recent() { tail -n "${1:-5}" "$RUN/change.log" | cut -f1,3-6 | sed 's/^/    /'; }
settle() { sleep 1; }

mkdir -p "$TREE/etc" "$OTHER"
echo "hello" >"$TREE/a.txt"
echo "obsolete" >"$TREE/old"
echo "token=42" >"$TREE/secret"
printf 'listen 80\n' >"$TREE/etc/server.conf"
printf 'scan_interval=3600\n' >"$DEMO/integrityd.conf"
mkdir -p "$RUN" && cp "$DEMO/integrityd.conf" "$RUN/"

step "1. Start the daemon (double fork, pid file, daemon.out)"
show L start --watch "$TREE"
show "ps -o pid,ppid,sid,tty,comm -p \$(cat '$RUN/integrityd.pid')"
show ls -l "$RUN"
recent 1

step "2. Create, edit, delete, chmod - each logged within a fraction of a second"
show "echo hi >> '$TREE/a.txt'; touch '$TREE/new'; rm '$TREE/old'; chmod 600 '$TREE/secret'"
settle
recent 4
show R

step "3. Stealth edit: change bytes, restore size and mtime with touch -r"
show "ls -l --time-style=full-iso '$TREE/etc/server.conf'"
show "touch -r '$TREE/etc/server.conf' '$DEMO/ref'; printf 'listen 81\n' > '$TREE/etc/server.conf'; touch -r '$DEMO/ref' '$TREE/etc/server.conf'"
show "ls -l --time-style=full-iso '$TREE/etc/server.conf'"
settle
recent 1

step "4. Live control: pause/resume, add a root, on-demand rescan with SIGUSR1"
show C pause
show "echo during-pause >> '$TREE/a.txt'"
settle
show C resume
recent 3
echo "o" >"$OTHER/o.txt"
show C add "$OTHER"
show "echo changed >> '$OTHER/o.txt'"
settle
recent 1
show "kill -USR1 \$(cat '$RUN/integrityd.pid')"
settle
recent 1
show C status

step "5. Stop, edit while stopped, start again: offline changes are reported"
show L stop
show "echo offline-edit >> '$TREE/a.txt'; rm '$TREE/new'"
show L start
settle
recent 4

step "6. Tamper with the log: verify-log names the record"
show L stop
show "'$INTEGRITYD' verify-log --rundir '$RUN'"
show "sed -n 2p '$RUN/change.log' | cut -f1,3,4"
show "sed -i '2s/\tALERT\tMODIFIED\t/\tINFO\tMODIFIED\t/' '$RUN/change.log'"
show "sed -n 2p '$RUN/change.log' | cut -f1,3,4"
show "'$INTEGRITYD' verify-log --rundir '$RUN'; echo \"exit code \$?\""
if "$INTEGRITYD" verify-log --rundir "$RUN" >/dev/null; then
    echo "demo error: tampering was not detected" >&2
    exit 1
fi

step "Summary report"
show R --format text
show R --format html
echo
echo "demo finished (temp files in $DEMO are removed on exit)"
