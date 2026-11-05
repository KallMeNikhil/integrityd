#!/bin/bash
. "$(dirname "$0")/lib.sh"

mkdir -p "$TREE/d"
echo data >"$TREE/d/file"
ln -s loop2 "$TREE/loop1"
ln -s loop1 "$TREE/loop2"
ln -s /nonexistent/x "$TREE/dangling"
ln -s .. "$TREE/d/up"
mkfifo "$TREE/fifo"

out=$(timeout 10 "$INTEGRITYD" check --scan-only --rundir "$RUN" --watch "$TREE") || fail "scan-only failed/hung (rc=$?)"
echo "$out" | awk -F'\t' -v p="$TREE/fifo" '$2==p && $1=="o"' | grep -q . || fail "fifo not type o"
echo "$out" | awk -F'\t' -v p="$TREE/loop1" '$2==p && $1=="l" && $13=="loop2"' | grep -q . || fail "loop1"
echo "$out" | awk -F'\t' -v p="$TREE/dangling" '$2==p && $1=="l" && $13=="/nonexistent/x"' | grep -q . || fail "dangling"
echo "$out" | awk -F'\t' -v p="$TREE/d/up" '$2==p && $1=="l"' | grep -q . || fail "dir symlink not recorded as link"
[ "$(echo "$out" | wc -l)" -eq 8 ] || fail "expected 8 records, got $(echo "$out" | wc -l)"
sorted=$(echo "$out" | cut -f2 | LC_ALL=C sort -c 2>&1) || fail "output not sorted: $sorted"
ok "special files scanned without hang"

do_baseline
timeout 10 "$INTEGRITYD" check --rundir "$RUN" --deep >"$CHK"
rc=$?
[ $rc -eq 0 ] || fail "deep check on unchanged special tree: rc=$rc $(cat "$CHK")"
ln -sfn loop1 "$TREE/dangling"
do_check --deep
[ "$(chk_count MODIFIED "$TREE/dangling" "new_target=loop1")" -eq 1 ] || fail "link retarget: $(cat "$CHK")"
ok "baseline + deep check with FIFO and loops; link retarget -> MODIFIED"
