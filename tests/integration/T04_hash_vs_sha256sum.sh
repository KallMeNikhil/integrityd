#!/bin/bash
. "$(dirname "$0")/lib.sh"

files=()
for n in 0 1 55 56 63 64 65 100000; do
    head -c "$n" /dev/urandom >"$TREE/f$n"
    files+=("$TREE/f$n")
done
head -c $((100 * 1024 * 1024)) /dev/urandom >"$TREE/f100M"
files+=("$TREE/f100M")

mine=$("$INTEGRITYD" hash "${files[@]}") || fail "hash failed"
ref=$(sha256sum "${files[@]}")
[ "$mine" = "$ref" ] || fail "mismatch:
$mine
---
$ref"
ok "integrityd hash == sha256sum for ${#files[@]} files"

mkfifo "$TREE/fifo"
timeout 5 "$INTEGRITYD" hash "$TREE/missing" "$TREE/fifo" "$TREE/f1" >"$TMP/o" 2>"$TMP/e"
rc=$?
[ $rc -eq 1 ] || fail "expected exit 1, got $rc"
grep -q "f1" "$TMP/o" || fail "good file not printed"
grep -q "missing" "$TMP/e" && grep -q "fifo" "$TMP/e" || fail "errors not reported"
ok "errors reported, FIFO not opened"
