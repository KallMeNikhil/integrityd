#!/bin/bash
. "$(dirname "$0")/lib.sh"

printf 'password=hunter2\n' >"$TREE/secret.conf"
touch -d '2024-01-01 12:00:00' "$TREE/secret.conf"

stealth_edit() {
    touch -r "$TREE/secret.conf" "$TMP/ref"
    before=$(ls -l --time-style=full-iso "$TREE/secret.conf")
    printf '%s\n' "$1" >"$TREE/secret.conf"
    touch -r "$TMP/ref" "$TREE/secret.conf"
    after=$(ls -l --time-style=full-iso "$TREE/secret.conf")
    [ "$before" = "$after" ] || fail "ls -l differs, not a stealth edit: $before / $after"
}

do_baseline
stealth_edit 'password=HUNTER2'
do_check
[ "$CRC" -eq 1 ] || fail "metadata check missed the stealth edit (rc=$CRC)"
[ "$(chk_count MODIFIED "$TREE/secret.conf" "stealth=1")" -eq 1 ] || fail "no stealth=1: $(cat "$CHK")"
ok "offline meta check: ls -l identical, still MODIFIED stealth=1"

do_baseline
stealth_edit 'password=hunter3'
do_check --deep
[ "$(chk_count MODIFIED "$TREE/secret.conf" "stealth=1")" -eq 1 ] || fail "deep: no stealth=1: $(cat "$CHK")"
ok "offline deep check: MODIFIED stealth=1"

do_baseline
start_daemon
stealth_edit 'password=letmein'
wait_log MODIFIED "$TREE/secret.conf" "stealth=1"
settle
[ "$(log_count MODIFIED "$TREE/secret.conf")" -eq 1 ] || fail "live stealth edit reported more than once"
ok "live: stealth edit caught by the hash (MODIFIED stealth=1, once)"
