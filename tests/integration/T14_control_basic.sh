#!/bin/bash
. "$(dirname "$0")/lib.sh"

echo a >"$TREE/a"
start_daemon --watch "$TREE"

out=$(ctl ping) || fail "ping rc=$?"
[ "$out" = "OK pong" ] || fail "ping: $out"
out=$(ctl version) && [ "$out" = "OK integrityd 1.0.0" ] || fail "version: $out"
out=$(ctl status) || fail "status rc=$?"
for k in state=running mode=hybrid roots=1 records=2 "watches=1 " uptime_s= last_scan= last_scan_ms= \
    alerts_total=0 log_seq= "pid=$DPID " worker=idle; do
    case $out in *"$k"*) ;; *) fail "status lacks '$k': $out" ;; esac
done
out=$(ctl list) && [ "$out" = "OK $TREE" ] || fail "list: $out"
ok "ping, version, status ($out), list"

out=$(ctl raw FROBNICATE)
[ $? -eq 2 ] && [ "$out" = "ERR 400 unknown command" ] || fail "unknown command: $out"
out=$(ctl raw TAIL 999)
[ $? -eq 2 ] && case $out in "ERR 422 "*) true ;; *) false ;; esac || fail "bad arg: $out"
out=$(ctl raw ADD)
[ $? -eq 2 ] || fail "ADD without path accepted: $out"
out=$(ctl add relative/../nowhere)
[ $? -eq 2 ] || fail "ADD of a missing path accepted: $out"
ok "ERR 400 unknown command, ERR 422 bad arguments"

head -c 6000 /dev/zero | tr '\0' 'x' >"$TMP/long"
echo >>"$TMP/long"
cat "$TMP/long" >"$RUN/control.fifo"
printf 'garbage\n\n   \n' >"$RUN/control.fifo"
out=$(ctl ping) && [ "$out" = "OK pong" ] || fail "daemon stopped answering after flood: $out"
ok "over-long and malformed lines ignored"

kill -USR2 "$DPID" && wait_log STATUS
out=$(ctl tail 2) || fail "tail rc=$?"
[ "$(echo "$out" | wc -l)" -eq 2 ] || fail "tail 2 returned: $out"
case $out in "OK 1"$'\t'*DAEMON_START*STATUS*) ;; *) fail "tail content: $out" ;; esac
ls "$RUN/reply" | grep -q . && fail "reply FIFOs left behind"
[ "$(stat -c %a "$RUN/control.fifo")" = 600 ] && [ "$(stat -c %a "$RUN")" = 700 ] || fail "run dir/FIFO modes"
ok "tail, reply FIFOs cleaned up, modes 0700/0600"

ctl shutdown >/dev/null || fail "shutdown"
wait "$DPID"
rc=$?
DPID=
[ $rc -eq 0 ] || fail "daemon exit code $rc"
[ ! -e "$RUN/control.fifo" ] || fail "FIFO not removed"
out=$(ctl ping 2>&1)
[ $? -eq 1 ] || fail "ping to a stopped daemon: $out"
ok "SHUTDOWN: exit 0, FIFO removed; client reports no daemon"
