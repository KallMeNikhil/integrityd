#!/bin/bash
set -u
here=$(cd "$(dirname "$0")" && pwd)
: "${INTEGRITYD:?set INTEGRITYD to the integrityd binary}"
export INTEGRITYD
export SCRIPTS="$here/../scripts"

if [ $# -gt 0 ]; then
    tests=()
    for t in "$@"; do tests+=("$here"/integration/"$t"*.sh); done
else
    tests=("$here"/integration/T*.sh)
fi

pass=0 fail=0 failed=""
for t in "${tests[@]}"; do
    [ -e "$t" ] || continue
    name=$(basename "$t" .sh)
    out=$(timeout 180 bash "$t" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        pass=$((pass + 1))
        printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1))
        failed="$failed $name"
        printf 'FAIL %s (rc=%d)\n%s\n' "$name" "$rc" "$out" | sed 's/^/    /'
    fi
done
printf 'integration: %d passed, %d failed%s\n' "$pass" "$fail" "${failed:+ ($failed )}"
[ $fail -eq 0 ]
