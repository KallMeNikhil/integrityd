#!/bin/bash
. "$(dirname "$0")/lib.sh"

mkdir -p "$TREE/sub" "$TREE/locked"
echo a >"$TREE/sub/a"
echo s >"$TREE/locked/s"
echo r >"$TREE/readme"
echo big >"$TREE/big"
printf 'scan_interval=1\ndeep_every=1000\nhash_max_bytes=1000\nexclude=*.tmp\nexclude=cache/\n' >"$RUN/integrityd.conf"
start_daemon --watch "$TREE"

mkfifo "$TREE/pipe"
wait_log CREATED "$TREE/pipe" "type=o"
ctl ping >/dev/null || fail "daemon blocked on the FIFO"
ok "FIFO created live: CREATED type=o, daemon responsive"

rm "$TREE/readme" && mkdir "$TREE/readme"
wait_log TYPE_CHANGED "$TREE/readme" "old_type=f;new_type=d"
ok "file -> directory: TYPE_CHANGED"

echo t >"$TREE/sub/x.tmp"
mkdir "$TREE/cache" && echo c >"$TREE/cache/c"
echo marker >"$TREE/marker"
wait_log CREATED "$TREE/marker"
settle
[ "$(awk -F'\t' 'index($5,".tmp") || index($5,"/cache")' "$LOG" | wc -l)" -eq 0 ] || fail "excluded path reported"
ok "excluded patterns (*.tmp, cache/) never reported"

head -c 5000 /dev/zero >"$TREE/big"
wait_log MODIFIED "$TREE/big"
[ "$(log_count MODIFIED "$TREE/big" "new_sha=-")" -eq 1 ] || fail "file over hash_max_bytes was hashed"
ok "file over hash_max_bytes: MODIFIED basis on metadata, sha '-'"

if [ "$(id -u)" -ne 0 ]; then
    chmod 000 "$TREE/sub/a"
    wait_log UNREADABLE "$TREE/sub/a" "errno=13"
    sleep 2.5
    [ "$(log_count UNREADABLE "$TREE/sub/a")" -eq 1 ] || fail "UNREADABLE logged more than once"
    chmod 644 "$TREE/sub/a"
    wait_count PERMS "$TREE/sub/a" "" 2
    [ "$(log_count MODIFIED "$TREE/sub/a")" -eq 0 ] || fail "unreadable->readable reported as MODIFIED"
    chmod 000 "$TREE/sub/a"
    wait_count UNREADABLE "$TREE/sub/a" "" 2
    chmod 644 "$TREE/sub/a"
    ok "unreadable file: UNREADABLE once, PERMS both ways, re-armed after it cleared"

    chmod 000 "$TREE/locked"
    wait_log UNREADABLE "$TREE/locked"
    sleep 1.5
    [ "$(log_count DELETED "$TREE/locked/s")" -eq 0 ] || fail "children of an unreadable dir reported DELETED"
    chmod 755 "$TREE/locked"
    ok "unreadable directory: UNREADABLE, children not DELETED"
fi

stop_daemon
OTHER=$TMP/other
mkdir -p "$OTHER/in"
echo o >"$OTHER/in/o"
start_daemon --watch "$OTHER"
rm -rf "$OTHER"
wait_log DELETED "$OTHER/in/o"
wait_log DELETED "$OTHER"
mkdir -p "$OTHER" && echo back >"$OTHER/back"
wait_log CREATED "$OTHER/back" "" 5
sleep 1.5
echo again >>"$OTHER/back"
wait_log MODIFIED "$OTHER/back"
ok "root deleted: DELETED for its records; re-created: reported and watched again"

echo v1 >"$TMP/single.conf"
ctl add "$TMP/single.conf" >/dev/null || fail "add file root"
echo v2 >>"$TMP/single.conf"
wait_log MODIFIED "$TMP/single.conf"
echo v3 >"$TMP/single.tmp2" && mv "$TMP/single.tmp2" "$TMP/single.conf"
wait_count MODIFIED "$TMP/single.conf" "" 2
echo v4 >>"$TMP/single.conf"
wait_count MODIFIED "$TMP/single.conf" "" 3
ok "file root: modified, replaced atomically, still watched"
