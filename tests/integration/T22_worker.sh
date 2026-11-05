#!/bin/bash
. "$(dirname "$0")/lib.sh"

for i in $(seq 1 20); do echo "$i" >"$TREE/f$i"; done
truncate -s 400M "$TREE/big"
printf 'scan_interval=3600\nworker_min_records=0\nstartup_check=none\n' >>"$RUN/integrityd.conf"
start_daemon --watch "$TREE"

out=$(ctl deepverify) || fail "deepverify: $out"
case $out in "OK worker started pid="*) ;; *) fail "no worker started: $out" ;; esac
wpid=${out#*pid=}
[ "$(ps -o ppid= -p "$wpid" | tr -d ' ')" = "$DPID" ] || fail "worker is not a child of the daemon"
ctl status | grep -q "worker=running" || fail "status does not show the worker"
out=$(ctl deepverify)
[ $? -eq 2 ] || fail "second concurrent worker allowed: $out"
t0=$(date +%s%N)
ctl ping >/dev/null || fail "daemon unresponsive during the deep pass"
echo live >"$TREE/during"
wait_log CREATED "$TREE/during"
ms=$((($(date +%s%N) - t0) / 1000000))
ok "worker pid $wpid running; ping + live event handled in ${ms} ms meanwhile; only one worker"

wait_log SCAN_END "" "kind=deep;worker=1" 60
ctl status | grep -q "worker=idle" || fail "worker not idle after finishing"
[ -z "$(ps -o stat= -p "$wpid" 2>/dev/null)" ] || fail "worker not reaped (zombie?)"
ok "worker finished: SCAN_END kind=deep, reaped, idle"

out=$(ctl deepverify) || fail "deepverify 2: $out"
wpid=${out#*pid=}
sleep 0.3
kill -9 "$wpid"
wait_log ERROR "" "what=worker" 10
[ "$(log_count ERROR "" "signal=9")" -eq 1 ] || fail "worker death not described"
kill -0 "$DPID" || fail "daemon died with its worker"
ctl ping >/dev/null || fail "daemon unresponsive after worker death"
ctl status | grep -q "worker=idle" || fail "worker slot not freed"
out=$(ctl deepverify) || fail "cannot start a new worker after a crash: $out"
ok "killed worker: ERROR what=worker signal=9, daemon alive, new worker possible"
