#!/bin/bash
. "$(dirname "$0")/lib.sh"

echo keep >"$TREE/mod"
echo bye >"$TREE/del"
echo perm >"$TREE/perm"
start_daemon --watch "$TREE"

out=$(ctl pause) && [ "$out" = "OK paused" ] || fail "pause: $out"
ctl status | grep -q "state=paused" || fail "status not paused"
echo more >>"$TREE/mod"
rm "$TREE/del"
chmod 600 "$TREE/perm"
echo new >"$TREE/new"
mkdir -p "$TREE/dir/sub" && echo deep >"$TREE/dir/sub/deep"
settle
for ev in MODIFIED DELETED PERMS CREATED; do
    [ "$(log_count "$ev")" -eq 0 ] || fail "$ev reported while paused"
done
out=$(ctl rescan)
[ $? -eq 2 ] || fail "rescan while paused should be refused: $out"
ok "paused: no alerts while files change"

out=$(ctl resume) || fail "resume rc=$?"
for spec in "MODIFIED mod" "DELETED del" "PERMS perm" "CREATED new" "CREATED dir/sub/deep"; do
    set -- $spec
    [ "$(log_count "$1" "$TREE/$2" "during_pause=1")" -eq 1 ] || fail "$1 $2 not reported once with during_pause=1"
done
[ "$(log_count PAUSED)" -eq 1 ] && [ "$(log_count RESUMED)" -eq 1 ] || fail "PAUSED/RESUMED not logged"
ok "resume: every change reported once with during_pause=1 ($out)"

echo after >>"$TREE/mod"
wait_count MODIFIED "$TREE/mod" "" 2
[ "$(awk -F'\t' -v p="$TREE/mod" '$4=="MODIFIED" && $5==p && $6 !~ /during_pause/' "$LOG" | wc -l)" -eq 1 ] ||
    fail "live detection did not resume"
ok "live detection active again after resume"
