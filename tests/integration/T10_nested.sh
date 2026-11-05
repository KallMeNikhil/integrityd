#!/bin/bash
. "$(dirname "$0")/lib.sh"

mkdir -p "$TREE/l1/l2/l3"
echo deep >"$TREE/l1/l2/l3/file"
do_baseline
echo deeper >>"$TREE/l1/l2/l3/file"
echo new >"$TREE/l1/l2/l3/new"
do_check
[ "$(chk_count MODIFIED "$TREE/l1/l2/l3/file")" -eq 1 ] || fail "nested MODIFIED missing: $(cat "$CHK")"
[ "$(chk_count CREATED "$TREE/l1/l2/l3/new")" -eq 1 ] || fail "nested CREATED missing"
ok "offline: nested changes"

do_baseline
start_daemon
echo deepest >>"$TREE/l1/l2/l3/file"
wait_log_timed 1500 MODIFIED "$TREE/l1/l2/l3/file"
rm "$TREE/l1/l2/l3/new"
wait_log DELETED "$TREE/l1/l2/l3/new"
ok "live: nested MODIFIED (${LATENCY_MS} ms) and DELETED, watched 3 levels down"
