#!/bin/bash
. "$(dirname "$0")/lib.sh"

echo x >"$TREE/x"
start_daemon --watch "$TREE"
mkdir -p "$TREE/new/a/b"
for i in $(seq 1 40); do echo "$i" >"$TREE/new/a/b/f$i"; done
for i in $(seq 1 10); do echo "$i" >"$TREE/new/top$i"; done
mkdir "$TREE/new/a/b/c" && echo late >"$TREE/new/a/b/c/late"
wait_log CREATED "$TREE/new/a/b/c/late" "" 5
settle
n=$(awk -F'\t' -v p="$TREE/new/" '$4=="CREATED" && index($5,p)==1 && $6 ~ /^type=f/' "$LOG" | wc -l)
[ "$n" -eq 51 ] || fail "expected 51 files CREATED under new/, got $n"
for d in new new/a new/a/b new/a/b/c; do
    [ "$(log_count CREATED "$TREE/$d" "type=d")" -eq 1 ] || fail "directory $d not reported once"
done
ok "live: 51 files and 4 directories in a freshly populated subtree reported exactly once"

echo more >>"$TREE/new/a/b/f7"
wait_log MODIFIED "$TREE/new/a/b/f7"
ok "live: new subtree is watched"

mkdir -p "$TMP/outside/sub" && echo o >"$TMP/outside/sub/o"
mv "$TMP/outside" "$TREE/moved-in"
wait_log CREATED "$TREE/moved-in/sub/o"
echo again >>"$TREE/moved-in/sub/o"
wait_log MODIFIED "$TREE/moved-in/sub/o"
mv "$TREE/moved-in" "$TMP/gone"
wait_log DELETED "$TREE/moved-in/sub/o"
echo z >>"$TMP/gone/sub/o"
settle
[ "$(log_count MODIFIED "$TREE/moved-in/sub/o")" -eq 1 ] || fail "change outside the tree was reported"
ok "live: directory moved in is watched; moved out -> DELETED and unwatched"
