#!/bin/bash
set -u

here=$(cd "$(dirname "$0")" && pwd)
INTEGRITYD=${INTEGRITYD:-}
if [ -z "$INTEGRITYD" ]; then
    if [ -x "$here/../build/integrityd" ]; then INTEGRITYD=$here/../build/integrityd
    else INTEGRITYD=integrityd; fi
fi
RUNDIR=${INTEGRITYD_RUNDIR:-}
if [ "${1:-}" = "--rundir" ]; then
    RUNDIR=${2:?--rundir needs a directory}
    shift 2
fi
if [ -z "$RUNDIR" ]; then
    if [ "$(id -u)" -eq 0 ]; then RUNDIR=/var/lib/integrityd; else RUNDIR=$HOME/.integrityd; fi
fi
PIDFILE=$RUNDIR/integrityd.pid
ERRF=''

cleanup() { [ -n "$ERRF" ] && rm -f "$ERRF"; }
trap cleanup EXIT INT TERM

live_pid() {
    local pid
    pid=$(cat "$PIDFILE" 2>/dev/null) || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = integrityd ] || return 1
    echo "$pid"
}

start() {
    local pid tries=0
    if pid=$(live_pid); then
        echo "already running (pid $pid)"
        return 3
    fi
    ERRF=$(mktemp)
    "$INTEGRITYD" daemon --rundir "$RUNDIR" "$@" 2>"$ERRF"
    local rc=$?
    if [ $rc -ne 0 ]; then
        cat "$ERRF" >&2
        return $rc
    fi
    until [ -s "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; do
        tries=$((tries + 1))
        if [ $tries -gt 50 ]; then
            echo "integrityd did not start; last lines of $RUNDIR/daemon.out:" >&2
            tail -n 5 "$RUNDIR/daemon.out" >&2 2>/dev/null
            return 1
        fi
        sleep 0.1
    done
    echo "started pid $(cat "$PIDFILE")"
}

stop() {
    local pid waited=0
    if ! pid=$(live_pid); then
        echo "not running"
        return 0
    fi
    kill -TERM "$pid"
    while kill -0 "$pid" 2>/dev/null; do
        waited=$((waited + 1))
        if [ $waited -gt 100 ]; then
            echo "pid $pid still running after 10 s (not killed)" >&2
            return 1
        fi
        sleep 0.1
    done
    echo "stopped pid $pid"
}

status() {
    local pid
    if ! pid=$(live_pid); then
        echo "not running"
        return 1
    fi
    echo "running pid $pid, up $(ps -o etime= -p "$pid" | tr -d ' ')"
    "$here/integrityctl" --rundir "$RUNDIR" status
}

cmd=${1:-}
[ $# -gt 0 ] && shift
case $cmd in
    start) start "$@" ;;
    stop) stop ;;
    restart) stop && start "$@" ;;
    status) status ;;
    *)
        echo "usage: launch.sh [--rundir D] start|stop|restart|status [daemon options]" >&2
        exit 2 ;;
esac
