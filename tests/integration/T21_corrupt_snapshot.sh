#!/bin/bash
. "$(dirname "$0")/lib.sh"

echo a >"$TREE/a"
mkdir "$TREE/d" && echo b >"$TREE/d/b"
do_baseline
cp "$RUN/snapshot.db" "$TMP/snap.orig"
sed -i '0,/\t0644\t/s//\t0600\t/' "$RUN/snapshot.db"
cmp -s "$RUN/snapshot.db" "$TMP/snap.orig" && fail "sed did not modify the snapshot"

"$INTEGRITYD" check --rundir "$RUN" >/dev/null 2>"$TMP/err"
[ $? -eq 4 ] || fail "check on a corrupt snapshot should exit 4"
grep -q "checksum" "$TMP/err" || fail "check did not explain: $(cat "$TMP/err")"
ok "check: corrupt snapshot -> exit 4 ($(cat "$TMP/err"))"

start_daemon --watch "$TREE"
[ "$(log_count ERROR "" "what=snapshot_corrupt")" -eq 1 ] || fail "no ERROR snapshot_corrupt"
ls "$RUN"/snapshot.db.corrupt.* >/dev/null 2>&1 || fail "corrupt snapshot not kept aside"
[ "$(log_count DAEMON_START "" "baseline=initial")" -eq 1 ] || fail "no re-baseline"
[ "$(log_count CREATED)" -eq 0 ] || fail "re-baseline produced alerts"
echo live >>"$TREE/d/b"
wait_log MODIFIED "$TREE/d/b"
stop_daemon
do_check
[ "$CRC" -eq 0 ] || fail "new snapshot not valid"
ok "daemon: corrupt snapshot renamed, ERROR logged, fresh baseline works"

head -c 100 "$RUN/snapshot.db" >"$TMP/t" && cp "$TMP/t" "$RUN/snapshot.db"
start_daemon
[ "$(log_count ERROR "" "what=snapshot_corrupt")" -eq 2 ] || fail "truncated snapshot not detected"
stop_daemon
ok "truncated snapshot detected"
