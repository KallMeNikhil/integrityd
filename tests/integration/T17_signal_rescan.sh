#!/bin/bash
. "$(dirname "$0")/lib.sh"

echo v1 >"$TREE/f"
start_daemon --watch "$TREE" --mode scan
ctl status | grep -q "mode=scan" || fail "not in scan mode"
ctl status | grep -q "watches=0 " || fail "scan mode must not use inotify"
echo v2 >>"$TREE/f"
settle
[ "$(log_count MODIFIED)" -eq 0 ] || fail "scan mode reported without a scan"
kill -USR1 "$DPID"
wait_log SCAN_END "" "kind=signal"
[ "$(log_count MODIFIED "$TREE/f")" -eq 1 ] || fail "USR1 rescan missed the change"
ok "kill -USR1: SCAN_END kind=signal, MODIFIED found"

echo v3 >>"$TREE/f"
out=$(ctl kill-rescan) || fail "kill-rescan: $out"
wait_log MODIFIED "$TREE/f" "new_size=9"
echo v4 >>"$TREE/f"
out=$(ctl rescan "$TREE") || fail "rescan: $out"
case $out in *"changes=1"*) ;; *) fail "RESCAN reply: $out" ;; esac
ok "integrityctl kill-rescan and rescan PATH ($out)"

kill -USR2 "$DPID"
wait_log STATUS "" "state=running"
ok "kill -USR2: STATUS line logged"

echo "mode=hybrid" >>"$RUN/integrityd.conf"
stop_daemon
start_daemon --watch "$TREE"
ctl status | grep -q "mode=hybrid" || fail "config mode not applied"
printf 'scan_interval=3600\nmode=scan\n' >"$RUN/integrityd.conf"
kill -HUP "$DPID"
wait_log CONFIG_RELOAD "" "mode=scan"
ctl status | grep -q "watches=0 " || fail "SIGHUP to scan mode kept inotify watches"
printf 'scan_interval=3600\nmode=watch\n' >"$RUN/integrityd.conf"
out=$(ctl reload) || fail "reload: $out"
ctl status | grep -q "mode=watch" || fail "RELOAD did not switch mode"
echo live >"$TREE/live"
wait_log CREATED "$TREE/live"
echo "bogus line" >>"$RUN/integrityd.conf"
out=$(ctl reload)
[ $? -eq 2 ] || fail "bad config accepted: $out"
ctl status | grep -q "mode=watch" || fail "bad reload changed the config"
ok "SIGHUP and RELOAD switch modes; a bad config is rejected and the old one kept"
