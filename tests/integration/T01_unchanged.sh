#!/bin/bash
. "$(dirname "$0")/lib.sh"

mkdir -p "$TREE/a/b"
echo one >"$TREE/one"
echo two >"$TREE/a/b/two"
ln -s one "$TREE/link"
do_baseline
do_check
[ "$CRC" -eq 0 ] || fail "check rc=$CRC: $(cat "$CHK")"
[ ! -s "$CHK" ] || fail "unexpected output: $(cat "$CHK")"
do_check --deep
[ "$CRC" -eq 0 ] || fail "deep check rc=$CRC"
ok "unchanged tree: exit 0 (meta and deep)"

[ "$(log_count REBASELINE)" -eq 1 ] || fail "baseline not logged"
"$INTEGRITYD" check --rundir "$TMP/none" >/dev/null 2>&1
[ $? -eq 2 ] || fail "check without snapshot should exit 2"
ok "baseline logged; missing snapshot -> exit 2"
