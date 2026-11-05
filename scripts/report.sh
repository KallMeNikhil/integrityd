#!/bin/bash
set -u
export LC_ALL=C

RUNDIR=${INTEGRITYD_RUNDIR:-}
SINCE='' UNTIL='' FORMAT=oneline EVENTS=0
TMPF=''

usage() {
    echo 'usage: report.sh [--rundir D] [--since "YYYY-MM-DD[ HH:MM]"] [--until ...] [--format oneline|text|html] [--events]'
}
cleanup() { [ -n "$TMPF" ] && rm -f "$TMPF"; }
trap cleanup EXIT INT TERM

while [ $# -gt 0 ]; do
    case $1 in
        --rundir) RUNDIR=$2; shift 2 ;;
        --since) SINCE=$2; shift 2 ;;
        --until) UNTIL=$2; shift 2 ;;
        --format) FORMAT=$2; shift 2 ;;
        --events) EVENTS=1; shift ;;
        -h | --help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
case $FORMAT in oneline | text | html) ;; *) usage >&2; exit 2 ;; esac
if [ -z "$RUNDIR" ]; then
    if [ "$(id -u)" -eq 0 ]; then RUNDIR=/var/lib/integrityd; else RUNDIR=$HOME/.integrityd; fi
fi
LOG=$RUNDIR/change.log

norm_time() { printf '%s' "$1" | sed 's/ /T/'; }

records() {
    local f
    { ls -v "$LOG".[0-9]* 2>/dev/null | tac; echo "$LOG"; } |
        while IFS= read -r f; do cat "$f" 2>/dev/null; done |
        awk -F'\t' -v since="$(norm_time "$SINCE")" -v until="$(norm_time "$UNTIL")" -v events="$EVENTS" '
            /^#/ || NF != 7 { next }
            since != "" && substr($2, 1, length(since)) < since { next }
            until != "" && substr($2, 1, length(until)) > until { next }
            $3 == "INFO" && !events { next }
            { print }'
}

count() { awk -F'\t' -v ev="$1" '$4 == ev { n++ } END { print n + 0 }' "$TMPF"; }

plural() { [ "$1" -eq 1 ] && printf '%s %s' "$1" "$2" || printf '%s %ss' "$1" "$2"; }

oneline() {
    local m c d p v range=''
    m=$(count MODIFIED) c=$(count CREATED) d=$(count DELETED) p=$(count PERMS) v=$(count MOVED)
    if [ -n "$SINCE" ] && [ -n "$UNTIL" ]; then range=" (since $SINCE until $UNTIL)"
    elif [ -n "$SINCE" ]; then range=" (since $SINCE)"
    elif [ -n "$UNTIL" ]; then range=" (until $UNTIL)"
    fi
    awk -v m="$m" -v c="$c" -v d="$d" -v p="$(plural "$p" "permission change")" -v v="$v" -v r="$range" \
        'BEGIN { printf "Report: %d modified, %d created, %d deleted, %s, %d moved%s\n", m, c, d, p, v, r }'
}

event_table() { cut -f4 "$TMPF" | sort | uniq -c | sort -rn; }

recent_alerts() {
    awk -F'\t' '$3 == "ALERT" { print "[" substr($2, 1, 19) "] " $4 " " $5 "|" $6 }' "$TMPF" | tail -n 20 |
        sed -E \
            -e 's/\|old_sha=[^;]*;new_sha=[^;]*;old_size=([0-9]+);new_size=([0-9]+)/ (size \1 -> \2, content changed/' \
            -e 's/;old_target=([^;]*);new_target=([^;]*)/, link \1 -> \2/' \
            -e 's/;stealth=1/, size and mtime unchanged: stealth edit/' \
            -e 's/;basis=meta/, judged by metadata/' \
            -e 's/\|old_mode=([0-7]+);new_mode=([0-7]+);old_uid=([0-9]+);new_uid=([0-9]+);old_gid=([0-9]+);new_gid=([0-9]+)/ (mode \1 -> \2, uid \3 -> \4, gid \5 -> \6/' \
            -e 's/ \(mode ([0-7]+) -> \1, / (/' -e 's/, uid ([0-9]+) -> \1//' -e 's/, gid ([0-9]+) -> \1//' \
            -e 's/\|type=(.);old_sha=[^;]*;old_size=([0-9]+)/ (type \1, was \2 bytes/' \
            -e 's/\|old_type=(.);new_type=(.)/ (type \1 -> \2/' \
            -e 's/;(during_pause|offline)=1/, \1/' \
            -e 's/\|[^|]*$//' \
            -e 's/ \(\(/ (/' -e '/ \([^)]*$/s/$/)/'
}

top_paths() {
    awk -F'\t' '$3 != "INFO" && $5 != "-" { print $5 }' "$TMPF" | sort | uniq -c | sort -rn | head -n 5
}

html_escape() { sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'; }

text_report() {
    oneline
    echo
    echo "Events by type:"
    event_table | awk '{ printf "  %-14s %6d\n", $2, $1 }'
    echo
    echo "Most recent alerts:"
    recent_alerts | sed 's/^/  /'
    echo
    echo "Most changed paths:"
    top_paths | awk '{ n = $1; sub(/^ *[0-9]+ /, ""); printf "  %4d  %s\n", n, $0 }'
}

html_report() {
    local out=$RUNDIR/report.html
    {
        cat <<EOF
<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>integrityd report</title>
<style>body{font-family:sans-serif;margin:2em}table{border-collapse:collapse}
td,th{border:1px solid #999;padding:.3em .6em;text-align:left}code{font-size:90%}</style></head>
<body><h1>integrityd report</h1>
<p>$(oneline | html_escape)</p>
<p>Generated $(date -u +%Y-%m-%dT%H:%M:%SZ) from $(printf '%s' "$LOG" | html_escape)</p>
<h2>Events by type</h2>
<table><tr><th>Event</th><th>Count</th></tr>
EOF
        event_table | while read -r n ev; do printf '<tr><td>%s</td><td>%s</td></tr>\n' "$ev" "$n"; done
        printf '</table>\n<h2>Most recent alerts</h2>\n<ul>\n'
        recent_alerts | html_escape | while IFS= read -r line; do printf '<li><code>%s</code></li>\n' "$line"; done
        printf '</ul>\n<h2>Most changed paths</h2>\n<table><tr><th>Changes</th><th>Path</th></tr>\n'
        top_paths | html_escape | while read -r n p; do printf '<tr><td>%s</td><td><code>%s</code></td></tr>\n' "$n" "$p"; done
        printf '</table>\n</body></html>\n'
    } >"$out"
    echo "wrote $out"
}

if [ ! -f "$LOG" ] && ! ls "$LOG".[0-9]* >/dev/null 2>&1; then
    echo "report.sh: no change log in $RUNDIR" >&2
    exit 1
fi
TMPF=$(mktemp)
records >"$TMPF"
case $FORMAT in
    oneline) oneline ;;
    text) text_report ;;
    html) html_report ;;
esac
