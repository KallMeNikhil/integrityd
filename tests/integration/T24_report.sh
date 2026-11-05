#!/bin/bash
. "$(dirname "$0")/lib.sh"

R() { "$SCRIPTS/report.sh" --rundir "$RUN" "$@"; }
Z=0000000000000000000000000000000000000000000000000000000000000000
rec() { printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" "$6" "$Z"; }
sha=aaaa
{
    rec 1 2026-10-30T10:00:00.000Z INFO DAEMON_START - baseline=initial
    rec 2 2026-10-30T10:05:00.000Z ALERT MODIFIED /w/a "old_sha=$sha;new_sha=$sha;old_size=10;new_size=20"
    rec 3 2026-10-30T10:06:00.000Z WARN CREATED /w/MODIFIED.txt "type=f;size=1;sha=$sha;mode=0644"
    rec 4 2026-10-30T10:07:00.000Z INFO LOG_ROTATE - bytes=1
} >"$LOG.1"
{
    rec 5 2026-10-31T09:30:00.000Z INFO LOG_ROTATE - cont=1
    rec 6 2026-10-31T09:31:00.000Z ALERT MODIFIED /w/a "old_sha=$sha;new_sha=$sha;old_size=20;new_size=25;stealth=1"
    rec 7 2026-10-31T09:32:00.000Z ALERT DELETED /w/b "type=f;old_sha=$sha;old_size=7"
    rec 8 2026-10-31T09:33:00.000Z ALERT PERMS /w/c "old_mode=0644;new_mode=0600;old_uid=1000;new_uid=1000;old_gid=1000;new_gid=1000"
    rec 9 2026-10-31T09:34:00.000Z WARN MOVED /w/d "from=/w/e;sha=$sha;size=3"
    rec 10 2026-10-31T10:00:00.000Z WARN CREATED /w/f "type=f;size=1;sha=$sha;mode=0644"
    rec 11 2026-10-31T10:01:00.000Z ALERT PERMS /w/g "old_mode=0755;new_mode=0700;old_uid=1000;new_uid=0;old_gid=1000;new_gid=1000"
    echo "# a comment line is ignored"
} >"$LOG"

expect() { [ "$1" = "$2" ] || fail "expected: $2
got:      $1"; }
expect "$(R)" "Report: 2 modified, 2 created, 1 deleted, 2 permission changes, 1 moved"
expect "$(R --since "2026-10-31 09:00")" \
    "Report: 1 modified, 1 created, 1 deleted, 2 permission changes, 1 moved (since 2026-10-31 09:00)"
expect "$(R --since "2026-10-31 09:00" --until "2026-10-31 09:33")" \
    "Report: 1 modified, 0 created, 1 deleted, 1 permission change, 0 moved (since 2026-10-31 09:00 until 2026-10-31 09:33)"
expect "$(R --until 2026-10-30)" "Report: 1 modified, 1 created, 0 deleted, 0 permission changes, 0 moved (until 2026-10-30)"
ok "oneline: counts across change.log.1 + change.log, name 'MODIFIED.txt' not miscounted, date filters"

txt=$(R --format text)
has() { printf '%s\n' "$txt" | grep -qF -- "$1" || fail "text report lacks: $1
$txt"; }
has "Report: 2 modified, 2 created"
has "Events by type:"
printf '%s\n' "$txt" | grep -qE '^  MODIFIED +2$' || fail "count table: $txt"
printf '%s\n' "$txt" | grep -qE '^  INFO|LOG_ROTATE' && fail "INFO events shown without --events"
has "[2026-10-31T09:31:00] MODIFIED /w/a (size 20 -> 25, content changed, size and mtime unchanged: stealth edit)"
has "[2026-10-31T09:32:00] DELETED /w/b (type f, was 7 bytes)"
has "[2026-10-31T09:33:00] PERMS /w/c (mode 0644 -> 0600)"
has "[2026-10-31T10:01:00] PERMS /w/g (mode 0755 -> 0700, uid 1000 -> 0)"
printf '%s\n' "$txt" | grep -qE '^ +2  /w/a$' || fail "top paths: $txt"
ok "text: count table, plain-language alerts, top paths"

R --format text --events | grep -qE '^  LOG_ROTATE +2$' || fail "--events does not include INFO"
ok "--events includes INFO events"

out=$(R --format html) || fail "html rc=$?"
[ -f "$RUN/report.html" ] || fail "no report.html"
grep -q '<td>MODIFIED</td><td>2</td>' "$RUN/report.html" || fail "html table"
grep -q 'Report: 2 modified' "$RUN/report.html" || fail "html summary"
grep -qi '<script' "$RUN/report.html" && fail "html contains script"
ok "html: $out"

rm -f "$LOG" "$LOG.1"
echo a >"$TREE/a" && echo b >"$TREE/b"
do_baseline
echo more >>"$TREE/a"; rm "$TREE/b"; echo c >"$TREE/c"; chmod 600 "$TREE/c"
do_check --update
expect "$(R)" "Report: $(log_count MODIFIED) modified, $(log_count CREATED) created, $(log_count DELETED) deleted, 0 permission changes, 0 moved"
ok "real log: $(R)"

"$SCRIPTS/report.sh" --rundir "$TMP/nolog" >/dev/null 2>&1 && fail "missing log should fail"
ok "missing log -> error"
