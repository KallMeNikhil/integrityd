#!/bin/bash
. "$(dirname "$0")/lib.sh"

printf 'log_max_bytes=3000\nlog_keep=3\n' >"$RUN/integrityd.conf"
do_baseline
for i in $(seq 1 12); do
    for j in 1 2 3; do echo "$i$j" >"$TREE/file_${i}_$j"; done
    do_check --update
done
[ -f "$LOG.1" ] || fail "no rotation happened"
[ -f "$LOG.4" ] && fail "log_keep=3 exceeded"
out=$("$INTEGRITYD" verify-log --rundir "$RUN") || fail "verify-log after rotation: $out"
last1=$(tail -n1 "$LOG.1" | cut -f1)
first=$(head -n1 "$LOG" | cut -f1)
[ "$first" -eq $((last1 + 1)) ] || fail "seq not continuous: $last1 -> $first"
tail -n1 "$LOG.1" | cut -f4 | grep -qx LOG_ROTATE || fail "old file does not end with LOG_ROTATE"
head -n1 "$LOG" | cut -f4,6 | grep -qx $'LOG_ROTATE\tcont=1' || fail "new file does not start with LOG_ROTATE cont=1"
ok "offline rotation: seq $last1 -> $first continuous, chain verifies ($out)"

rm -f "$LOG" "$LOG".[0-9]*
printf 'scan_interval=3600\nlog_max_bytes=2500\nlog_keep=3\n' >"$RUN/integrityd.conf"
start_daemon
for i in $(seq 1 25); do echo "$i" >"$TREE/live_$i"; done
settle
[ -f "$LOG.1" ] || fail "daemon did not rotate"
stop_daemon
out=$("$INTEGRITYD" verify-log --rundir "$RUN") || fail "verify-log after daemon rotation: $out"
n=$(cat "$LOG".[0-9]* "$LOG" | awk -F'\t' '$4=="CREATED"' | wc -l)
[ "$n" -eq 25 ] || fail "lost records across rotation: $n CREATED"
ok "daemon rotation: 25 CREATED across rotated files, chain verifies ($out)"
