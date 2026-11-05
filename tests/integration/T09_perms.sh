#!/bin/bash
. "$(dirname "$0")/lib.sh"

echo data >"$TREE/script.sh"
chmod 644 "$TREE/script.sh"
do_baseline
chmod 4755 "$TREE/script.sh"
do_check
[ "$CRC" -eq 1 ] || fail "check rc=$CRC"
[ "$(chk_count PERMS "$TREE/script.sh" "old_mode=0644;new_mode=4755")" -eq 1 ] || fail "no PERMS: $(cat "$CHK")"
[ "$(chk_count MODIFIED)" -eq 0 ] || fail "chmod reported as MODIFIED: $(cat "$CHK")"
do_check --deep
[ "$(chk_count MODIFIED)" -eq 0 ] || fail "deep: chmod reported as MODIFIED"
ok "offline: PERMS (setuid), no MODIFIED"

do_baseline
start_daemon
chmod 600 "$TREE/script.sh"
wait_log_timed 1500 PERMS "$TREE/script.sh"
[ "$(log_count PERMS "$TREE/script.sh" "old_mode=4755;new_mode=0600")" -eq 1 ] || fail "live PERMS detail"
mkdir "$TREE/dir" && sleep 0.5 && chmod 700 "$TREE/dir"
wait_log PERMS "$TREE/dir" "new_mode=0700"
settle
[ "$(log_count MODIFIED)" -eq 0 ] || fail "live chmod reported as MODIFIED"
ok "live: PERMS for file (${LATENCY_MS} ms) and directory, no MODIFIED"
