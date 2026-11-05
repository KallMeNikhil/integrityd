#!/bin/bash
. "$(dirname "$0")/lib.sh"

names=("with space" $'tab\there' $'new\nline' 'pct%41' 'semi;colon' 'eq=ual' $'ctl\001\177' $'latin\xe9\xff')
for n in "${names[@]}"; do printf '%s' "$n" >"$TREE/$n"; done
do_baseline
do_check
[ "$CRC" -eq 0 ] || fail "snapshot round trip: check after baseline rc=$CRC $(cat "$CHK")"
grep -q $'\t' <(cut -f2 "$RUN/snapshot.db" | grep -v '^#') && fail "raw tab inside a snapshot path field"
[ "$(grep -avc '^#' "$RUN/snapshot.db")" -eq $((${#names[@]} + 1)) ] || fail "a newline split a snapshot line"
ok "snapshot round trip: ${#names[@]} weird names, check clean"

esc() { printf '%s' "$1" | od -An -v -tx1 | tr -s ' \n' ' ' | awk '{
    for (i = 1; i <= NF; i++) { b = strtonum("0x" $i)
        if (b < 32 || b == 127 || b == 37) printf "%%%02X", b; else printf "%c", b } }'; }

start_daemon
for n in "${names[@]}"; do printf 'changed' >>"$TREE/$n"; done
wait_log MODIFIED "$(esc "$TREE/${names[7]}")" "" 5
settle
for n in "${names[@]}"; do
    [ "$(log_count MODIFIED "$(esc "$TREE/$n")")" -eq 1 ] || fail "log path for '$n' not escaped/unique"
done
[ "$(wc -l <"$LOG")" -eq "$(awk -F'\t' 'NF == 7' "$LOG" | wc -l)" ] || fail "a log line does not have 7 fields"
"$INTEGRITYD" verify-log --rundir "$RUN" >/dev/null || fail "chain broken"
ok "log: every weird name on one line, escaped exactly"

odd=$TMP/$'odd root\n%20'
mkdir -p "$odd"
echo x >"$odd/f"
out=$(ctl add "$odd") || fail "add weird root: $out"
listed=$(ctl list | grep -c 'odd%20root%0A%2520')
[ "$listed" -eq 1 ] || fail "LIST did not return the escaped root: $(ctl list)"
echo y >>"$odd/f"
wait_log MODIFIED "$(esc "$odd/f")"
out=$(ctl remove "$odd") || fail "remove weird root: $out"
ok "control: weird root added, listed escaped, watched, removed"
