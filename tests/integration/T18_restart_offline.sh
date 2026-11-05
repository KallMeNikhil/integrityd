#!/bin/bash
. "$(dirname "$0")/lib.sh"

L() { "$SCRIPTS/launch.sh" --rundir "$RUN" "$@"; }
echo keep >"$TREE/keep"
echo mod >"$TREE/mod"
echo del >"$TREE/del"
printf 'stealthy\n' >"$TREE/stealth"

out=$(L start --watch "$TREE") || fail "launch start: $out"
case $out in "started pid "*) ;; *) fail "start output: $out" ;; esac
pid=$(cat "$RUN/integrityd.pid")
[ "$(ps -o tty= -p "$pid" | tr -d ' ')" = "?" ] || fail "daemon has a controlling tty"
[ "$(ps -o sid= -p "$pid" | tr -d ' ')" != "$pid" ] || fail "daemon is a session leader (no second fork)"
[ "$(readlink "/proc/$pid/cwd")" = / ] || fail "daemon did not chdir /"
[ "$(readlink "/proc/$pid/fd/0")" = /dev/null ] || fail "stdin not /dev/null"
[ "$(readlink "/proc/$pid/fd/1")" = "$RUN/daemon.out" ] || fail "stdout not redirected to daemon.out"
out=$(L start --watch "$TREE")
[ $? -eq 3 ] || fail "second start not refused: $out"
"$INTEGRITYD" daemon --rundir "$RUN" --foreground >/dev/null 2>&1
[ $? -eq 3 ] || fail "second daemon on the same run dir did not exit 3"
"$INTEGRITYD" baseline --rundir "$RUN" --watch "$TREE" >/dev/null 2>&1
[ $? -eq 3 ] || fail "baseline while a daemon runs did not exit 3"
L status | grep -q "state=running" || fail "launch status"
ok "launch start: detached (no tty, not session leader, cwd /, fds redirected); duplicates exit 3"

out=$(L stop) || fail "stop: $out"
[ ! -e "$RUN/integrityd.pid" ] || fail "pid file left behind"
[ "$(log_count DAEMON_STOP)" -eq 1 ] || fail "no DAEMON_STOP"
cp "$RUN/snapshot.db" "$TMP/snap.before"

echo more >>"$TREE/mod"
rm "$TREE/del"
echo new >"$TREE/new"
chmod 600 "$TREE/keep"
touch -r "$TREE/stealth" "$TMP/ref"
printf 'STEALTHY\n' >"$TREE/stealth"
touch -r "$TMP/ref" "$TREE/stealth"

out=$(L start) || fail "restart: $out"
wait_log SCAN_END "" "kind=startup_deep"
for spec in "MODIFIED mod" "DELETED del" "CREATED new" "PERMS keep"; do
    set -- $spec
    [ "$(log_count "$1" "$TREE/$2" "offline=1")" -eq 1 ] || fail "offline $1 $2 not reported"
done
[ "$(log_count MODIFIED "$TREE/stealth" "stealth=1;offline=1")" -eq 1 ] || fail "offline stealth edit missed"
[ "$(log_count DAEMON_START "" "baseline=loaded")" -eq 1 ] || fail "snapshot not reloaded"
ok "offline changes reported on start with offline=1 (incl. stealth edit)"

L stop >/dev/null
out=$(L start)
wait_log_ready() { local i; for i in $(seq 1 50); do ctl ping >/dev/null 2>&1 && return 0; sleep 0.1; done; fail "no ping"; }
wait_log_ready
[ "$(log_count MODIFIED "$TREE/mod")" -eq 1 ] || fail "second restart re-reported old changes"
L stop >/dev/null
"$INTEGRITYD" verify-log --rundir "$RUN" >/dev/null || fail "chain broken across restarts"
ok "second restart: nothing re-reported; chain continuous across 3 daemon runs"

echo 999999 >"$RUN/integrityd.pid"
out=$(L start) || fail "start with stale pid file: $out"
L stop >/dev/null
ok "stale pid file removed on start"

echo "startup_check=none" >>"$RUN/integrityd.conf"
echo again >>"$TREE/mod"
L start >/dev/null && wait_log_ready
[ "$(log_count MODIFIED "$TREE/mod")" -eq 1 ] || fail "startup_check=none still reconciled"
L stop >/dev/null
ok "startup_check=none skips reconciliation"
