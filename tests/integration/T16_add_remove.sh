#!/bin/bash
. "$(dirname "$0")/lib.sh"

OTHER=$TMP/other
mkdir -p "$OTHER/sub"
echo o >"$OTHER/sub/existing"
echo t >"$TREE/t"
start_daemon --watch "$TREE"

out=$(ctl add "$OTHER") || fail "add: $out"
wait_log WATCH_ADDED "$OTHER"
[ "$(log_count CREATED "$OTHER/sub/existing")" -eq 0 ] || fail "ADD baseline was not silent"
ctl list | grep -qx "OK $OTHER" || fail "list lacks the new root"
echo changed >>"$OTHER/sub/existing"
wait_log MODIFIED "$OTHER/sub/existing"
echo n >"$OTHER/sub/new"
wait_log CREATED "$OTHER/sub/new"
ok "added root: silent baseline, then live MODIFIED/CREATED inside it"

out=$(ctl add "$OTHER/sub")
[ $? -eq 2 ] || fail "adding a root inside a root accepted: $out"
ok "nested ADD refused: $out"

out=$(ctl remove "$OTHER") || fail "remove: $out"
wait_log WATCH_REMOVED "$OTHER"
ctl list | grep -q "$OTHER" && fail "removed root still listed"
echo silent >>"$OTHER/sub/existing"
rm "$OTHER/sub/new"
settle
[ "$(log_count MODIFIED "$OTHER/sub/existing")" -eq 1 ] && [ "$(log_count DELETED "$OTHER/sub/new")" -eq 0 ] ||
    fail "removed root still reported"
grep -q "$OTHER" "$RUN/snapshot.db" && fail "removed root still in the snapshot"
ok "removed root: silence, gone from LIST and snapshot"

out=$(ctl add "$TMP") || fail "add parent: $out"
case $out in *"merged=1"*) ;; *) fail "parent ADD did not merge: $out" ;; esac
[ "$(ctl list | wc -l)" -eq 1 ] || fail "roots not merged"
echo z >"$TREE/z"
wait_log CREATED "$TREE/z"
ok "parent ADD merges the old root ($out)"
