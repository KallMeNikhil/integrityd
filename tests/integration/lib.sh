set -u
export LC_ALL=C
TMP=$(mktemp -d /tmp/integrityd-test.XXXXXX)
RUN=$TMP/run
TREE=$TMP/tree
mkdir -p "$RUN" "$TREE"
LOG=$RUN/change.log
DPID=
: "${INTEGRITYD:?}" "${SCRIPTS:?}"

cleanup() {
    local rc=$?
    if [ -n "$DPID" ] && kill -0 "$DPID" 2>/dev/null; then
        kill -TERM "$DPID" 2>/dev/null
        wait "$DPID" 2>/dev/null
    fi
    if cat "$TMP/fg.out" "$RUN/daemon.out" 2>/dev/null | grep -qE "Sanitizer|runtime error"; then
        echo "FAIL: sanitizer report in daemon output:" >&2
        cat "$TMP/fg.out" "$RUN/daemon.out" 2>/dev/null >&2
        rc=1
    fi
    chmod -R u+rwx "$TMP" 2>/dev/null
    rm -rf "$TMP"
    exit $rc
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    if [ -f "$LOG" ]; then echo "--- change.log" >&2; cat "$LOG" >&2; fi
    if [ -f "$TMP/fg.out" ]; then echo "--- daemon output" >&2; cat "$TMP/fg.out" >&2; fi
    exit 1
}
ok() { echo "ok: $*"; }

idx() { "$INTEGRITYD" "$@"; }
ctl() { "$SCRIPTS/integrityctl" --rundir "$RUN" "$@"; }

echo "scan_interval=3600" >"$RUN/integrityd.conf"

start_daemon() {
    local before i
    before=$(grep -c "integrityd: ready" "$TMP/fg.out" 2>/dev/null)
    "$INTEGRITYD" daemon --foreground --rundir "$RUN" "$@" >>"$TMP/fg.out" 2>&1 &
    DPID=$!
    for i in $(seq 1 150); do
        [ "$(grep -c "integrityd: ready" "$TMP/fg.out" 2>/dev/null)" -gt "${before:-0}" ] && return 0
        kill -0 "$DPID" 2>/dev/null || fail "daemon exited during startup"
        sleep 0.1
    done
    fail "daemon not ready"
}

wait_log_timed() {
    local t0 t1
    t0=$(date +%s%N)
    wait_log "$2" "$3" "" 5
    t1=$(date +%s%N)
    LATENCY_MS=$(((t1 - t0) / 1000000))
    [ "$LATENCY_MS" -le "$1" ] || fail "$2 $3 took ${LATENCY_MS} ms (limit $1)"
}

stop_daemon() {
    [ -n "$DPID" ] || return 0
    kill -TERM "$DPID" 2>/dev/null
    wait "$DPID" 2>/dev/null
    DPID=
}

log_count() {
    local f=${4:-$LOG}
    [ -f "$f" ] || { echo 0; return; }
    awk -F'\t' -v ev="$1" -v p="${2-}" -v d="${3-}" '
        $4 == ev && (p == "" || $5 == p) && (d == "" || index($6, d)) { n++ }
        END { print n + 0 }' "$f"
}

wait_log() {
    local t=${4:-5} i
    for i in $(seq 1 $((t * 10))); do
        [ "$(log_count "$1" "${2-}" "${3-}")" -gt 0 ] && return 0
        sleep 0.1
    done
    fail "timeout waiting for $1 ${2-} ${3-}"
}

wait_count() {
    local t=${5:-5} i
    for i in $(seq 1 $((t * 10))); do
        [ "$(log_count "$1" "$2" "$3")" -ge "$4" ] && return 0
        sleep 0.1
    done
    fail "timeout waiting for $4 x $1 $2 $3 (have $(log_count "$1" "$2" "$3"))"
}

expect_count() {
    local n
    n=$(log_count "$1" "$2" "$3")
    [ "$n" -eq "$4" ] || fail "expected $4 x $1 $2 $3, got $n"
}

settle() { sleep "${1:-1.5}"; }

CHK=$TMP/check.out
do_baseline() { "$INTEGRITYD" baseline --rundir "$RUN" --watch "$TREE" "$@" >/dev/null || fail "baseline failed"; }
do_check() { "$INTEGRITYD" check --rundir "$RUN" "$@" >"$CHK" 2>"$TMP/check.err"; CRC=$?; }
chk_count() { log_count "$1" "${2-}" "${3-}" "$CHK"; }
