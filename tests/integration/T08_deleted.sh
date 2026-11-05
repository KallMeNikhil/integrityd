#!/bin/bash
. "$(dirname "$0")/lib.sh"

echo doomed >"$TREE/doomed"
echo keep >"$TREE/keep"
echo later >"$TREE/later"
do_baseline
sha=$(sha256sum "$TREE/doomed" | cut -d' ' -f1)
rm "$TREE/doomed"
do_check
[ "$CRC" -eq 1 ] || fail "check rc=$CRC"
[ "$(chk_count DELETED "$TREE/doomed" "old_sha=$sha;old_size=7")" -eq 1 ] || fail "no DELETED: $(cat "$CHK")"
ok "offline: DELETED"

do_baseline
start_daemon
rm "$TREE/later"
wait_log_timed 1500 DELETED "$TREE/later"
ok "live: DELETED after ${LATENCY_MS} ms"
