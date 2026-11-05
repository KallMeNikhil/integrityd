#!/bin/bash
. "$(dirname "$0")/lib.sh"

echo a >"$TREE/a"
do_baseline
for i in 1 2 3 4 5; do
    echo "$i" >"$TREE/f$i"
    do_check --update
done
out=$("$INTEGRITYD" verify-log --rundir "$RUN") || fail "verify-log on clean log: $out"
case $out in "OK 6 head="*) ;; *) fail "unexpected: $out" ;; esac
ok "clean log: $out"

cp "$LOG" "$TMP/orig.log"
sed -i '4s|/f3\t|/fX\t|' "$LOG"
cmp -s "$LOG" "$TMP/orig.log" && fail "sed did not change the log"
out=$("$INTEGRITYD" verify-log --rundir "$RUN")
rc=$?
[ $rc -eq 4 ] || fail "tampered log: rc=$rc"
case $out in "BROKEN 4 "*) ;; *) fail "expected BROKEN 4, got: $out" ;; esac
ok "tampered record found: $out"

cp "$TMP/orig.log" "$LOG"
sed -i '3d' "$LOG"
out=$("$INTEGRITYD" verify-log --rundir "$RUN")
case $out in "BROKEN 3 "*) ;; *) fail "expected BROKEN 3 for deleted line, got: $out" ;; esac
ok "deleted record found: $out"

head -c -20 "$TMP/orig.log" >"$LOG"
out=$("$INTEGRITYD" verify-log --rundir "$RUN")
rc=$?
[ $rc -eq 4 ] || fail "truncated log: rc=$rc"
case $out in "TRUNCATED 6 "*) ;; *) fail "expected TRUNCATED 6, got: $out" ;; esac
echo z >"$TREE/z"
do_check --update
[ "$(log_count ERROR "" "what=log_partial_line")" -eq 1 ] || fail "partial line not logged"
out=$("$INTEGRITYD" verify-log --rundir "$RUN") || fail "after repair: $out"
ok "truncated tail: reported, dropped by next writer, chain OK ($out)"
