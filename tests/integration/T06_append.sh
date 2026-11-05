#!/bin/bash
. "$(dirname "$0")/lib.sh"

echo "line one" >"$TREE/f"
do_baseline
old=$(sha256sum "$TREE/f" | cut -d' ' -f1)
echo "line two" >>"$TREE/f"
new=$(sha256sum "$TREE/f" | cut -d' ' -f1)
do_check
[ "$CRC" -eq 1 ] || fail "check rc=$CRC"
[ "$(chk_count MODIFIED "$TREE/f" "old_sha=$old;new_sha=$new;old_size=9;new_size=18")" -eq 1 ] ||
    fail "MODIFIED detail wrong: $(cat "$CHK")"
ok "offline: MODIFIED with sizes and hashes"

do_baseline
start_daemon
echo "line three" >>"$TREE/f"
new2=$(sha256sum "$TREE/f" | cut -d' ' -f1)
wait_log_timed 1500 MODIFIED "$TREE/f"
[ "$(log_count MODIFIED "$TREE/f" "old_sha=$new;new_sha=$new2;old_size=18;new_size=29")" -eq 1 ] ||
    fail "live MODIFIED detail wrong"
ok "live: MODIFIED after ${LATENCY_MS} ms with sizes and hashes"
