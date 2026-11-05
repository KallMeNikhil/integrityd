#!/bin/bash
. "$(dirname "$0")/lib.sh"

mkdir -p "$TREE/sub"
echo payload >"$TREE/orig"
echo same-name >"$TREE/same"
echo victim >"$TREE/victim"
do_baseline
mv "$TREE/orig" "$TREE/sub/renamed"
do_check
[ "$(chk_count MOVED "$TREE/sub/renamed" "from=$TREE/orig")" -eq 1 ] || fail "no MOVED: $(cat "$CHK")"
[ "$(chk_count DELETED)" -eq 0 ] && [ "$(chk_count CREATED)" -eq 0 ] || fail "MOVED also reported as DELETED/CREATED"
ok "offline: rename -> MOVED"

do_baseline
rm "$TREE/same" && echo different >"$TREE/same"
rm "$TREE/victim" && echo other-content >"$TREE/reuse"
do_check
[ "$(chk_count MOVED)" -eq 0 ] || fail "delete+create paired as MOVED: $(cat "$CHK")"
[ "$(chk_count MODIFIED "$TREE/same")" -eq 1 ] || fail "same name, new content not MODIFIED"
[ "$(chk_count DELETED "$TREE/victim")" -eq 1 ] && [ "$(chk_count CREATED "$TREE/reuse")" -eq 1 ] ||
    fail "inode reuse not DELETED+CREATED: $(cat "$CHK")"
ok "offline: delete+create with different content is not MOVED"

do_baseline
start_daemon
mv "$TREE/sub/renamed" "$TREE/live-renamed"
wait_log_timed 1500 MOVED "$TREE/live-renamed"
[ "$(log_count MOVED "$TREE/live-renamed" "from=$TREE/sub/renamed")" -eq 1 ] || fail "live MOVED from= wrong"
rm "$TREE/reuse" && echo brand-new >"$TREE/fresh"
wait_log CREATED "$TREE/fresh"
wait_log DELETED "$TREE/reuse"
settle
[ "$(log_count MOVED "$TREE/fresh")" -eq 0 ] || fail "live delete+create paired as MOVED"
ok "live: rename -> MOVED (${LATENCY_MS} ms); delete+create -> DELETED+CREATED"
