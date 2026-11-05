#!/bin/bash
. "$(dirname "$0")/lib.sh"

echo base >"$TREE/base"
do_baseline
echo hello >"$TREE/new.txt"
do_check
[ "$CRC" -eq 1 ] || fail "check rc=$CRC"
[ "$(chk_count CREATED "$TREE/new.txt" "size=6")" -eq 1 ] || fail "no CREATED: $(cat "$CHK")"
[ "$(wc -l <"$CHK")" -eq 1 ] || fail "extra events: $(cat "$CHK")"
ok "offline: CREATED"

rm -f "$RUN/snapshot.db"
start_daemon --watch "$TREE"
[ "$(log_count DAEMON_START "" "baseline=initial")" -eq 1 ] || fail "no initial baseline"
[ "$(log_count CREATED)" -eq 0 ] || fail "initial baseline produced alerts"
[ -f "$RUN/integrityd.pid" ] && [ "$(cat "$RUN/integrityd.pid")" = "$DPID" ] || fail "pid file"
echo live >"$TREE/live.txt"
wait_log_timed 1500 CREATED "$TREE/live.txt"
ok "live: CREATED after ${LATENCY_MS} ms"
touch "$TREE/empty"
wait_log CREATED "$TREE/empty" "size=0"
stop_daemon
[ "$(log_count DAEMON_STOP)" -eq 1 ] || fail "no DAEMON_STOP"
[ ! -e "$RUN/integrityd.pid" ] || fail "pid file not removed"
"$INTEGRITYD" verify-log --rundir "$RUN" >/dev/null || fail "chain broken"
ok "live: empty file; graceful stop logged, pid file removed, chain OK"
