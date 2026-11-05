#!/bin/bash
. "$(dirname "$0")/lib.sh"

echo "version 1" >"$TREE/doc.txt"
echo "a" >"$TREE/burst"
echo "same" >"$TREE/same.txt"
start_daemon --watch "$TREE"

echo "version 2" >"$TREE/.doc.txt.tmp123"
mv "$TREE/.doc.txt.tmp123" "$TREE/doc.txt"
wait_log MODIFIED "$TREE/doc.txt"
settle
[ "$(log_count MODIFIED "$TREE/doc.txt")" -eq 1 ] || fail "atomic save gave more than one MODIFIED"
[ "$(awk -F'\t' 'index($5,".doc.txt.tmp")' "$LOG" | wc -l)" -eq 0 ] || fail "temp file leaked into the log"
ok "atomic save: exactly one MODIFIED, temp file invisible"

mv "$TREE/doc.txt" "$TREE/doc.txt~"
echo "version 3" >"$TREE/doc.txt"
rm "$TREE/doc.txt~"
wait_log MODIFIED "$TREE/doc.txt" "new_size=10"
settle
[ "$(log_count MODIFIED "$TREE/doc.txt")" -eq 2 ] || fail "vim-style save not exactly one more MODIFIED"
[ "$(log_count DELETED "$TREE/doc.txt")" -eq 0 ] || fail "vim-style save reported DELETED"
ok "vim-style save: one MODIFIED, no DELETED"

for i in $(seq 1 20); do echo "$i" >>"$TREE/burst"; done
wait_log MODIFIED "$TREE/burst"
settle
[ "$(log_count MODIFIED "$TREE/burst")" -eq 1 ] || fail "burst gave $(log_count MODIFIED "$TREE/burst") MODIFIED"
ok "burst of 20 appends: one MODIFIED"

cp "$TREE/same.txt" "$TREE/.same.tmp" && mv "$TREE/.same.tmp" "$TREE/same.txt"
wait_log REPLACED "$TREE/same.txt"
[ "$(log_count MODIFIED "$TREE/same.txt")" -eq 0 ] || fail "identical content reported MODIFIED"
ok "same content, new inode: REPLACED"
