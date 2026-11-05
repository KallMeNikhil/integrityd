#!/bin/bash
. "$(dirname "$0")/lib.sh"

HOOK=$TMP/hook.sh
OUT=$TMP/hook.out
cat >"$HOOK" <<EOF
#!/bin/bash
printf '%s|%s|%s|%s|%s|%s|%s\n' "\$#" "\$1" "\$2" "\$3" "\$4" "\$INTEGRITYD_DETAIL" "\$PATH" >>"$OUT"
EOF
chmod 700 "$HOOK"
echo a >"$TREE/a"
printf 'scan_interval=3600\nalert_hook=%s\nhook_min_severity=WARN\nhook_rate_per_sec=3\n' "$HOOK" >"$RUN/integrityd.conf"
cd "$TMP" || fail "cd"
start_daemon --watch "$TREE"

evil="$TREE/x; touch pwned \$(id)"
echo e >"$evil"
for i in $(seq 1 50); do [ -s "$OUT" ] && break; sleep 0.1; done
seq=$(log_count CREATED "$evil" >/dev/null; awk -F'\t' '$4=="CREATED" {print $1}' "$LOG")
line=$(head -n1 "$OUT")
IFS='|' read -r argc ev p sev s detail path <<<"$line"
[ "$argc" = 4 ] && [ "$ev" = CREATED ] && [ "$p" = "$evil" ] && [ "$sev" = WARN ] && [ "$s" = "$seq" ] ||
    fail "hook args wrong: $line (seq $seq)"
case $detail in "type=f;size=2;sha="*";mode=0644") ;; *) fail "INTEGRITYD_DETAIL: $detail" ;; esac
[ "$path" = /usr/bin:/bin ] || fail "hook PATH: $path"
[ -e "$TMP/pwned" ] && fail "file name was executed by a shell"
ok "hook: argv=[EVENT PATH SEVERITY SEQ], INTEGRITYD_DETAIL, PATH=/usr/bin:/bin, no shell"

: >"$OUT"
kill -USR2 "$DPID"
echo more >>"$TREE/a"
wait_log MODIFIED "$TREE/a"
for i in $(seq 1 30); do [ -s "$OUT" ] && break; sleep 0.1; done
grep -q '^4|MODIFIED|' "$OUT" || fail "ALERT not delivered: $(cat "$OUT")"
grep -q 'STATUS' "$OUT" && fail "INFO event delivered to the hook"
ok "severity filter: ALERT delivered, INFO not"

: >"$OUT"
for i in $(seq 1 15); do echo "$i" >"$TREE/burst$i"; done
wait_log ERROR "" "what=hook_rate" 5
n=$(wc -l <"$OUT")
dropped=$(awk -F'\t' '$4=="ERROR" && index($6,"what=hook_rate") {print $6}' "$LOG" | sed 's/.*hook_dropped=//' | head -n1)
[ "$n" -le 3 ] && [ "$dropped" -ge 10 ] && [ $((n + dropped)) -eq 15 ] || fail "rate limit: ran $n, dropped $dropped"
ok "rate limit: $n hook runs, $dropped dropped and reported as hook_dropped"

chmod 777 "$HOOK"
ctl reload >/dev/null
wait_log ERROR "" "what=hook_rejected"
[ "$(log_count ERROR "" "group- or world-writable")" -eq 1 ] || fail "world-writable hook not rejected"
: >"$OUT"
echo x >"$TREE/after-reject"
wait_log CREATED "$TREE/after-reject"
sleep 0.5
[ -s "$OUT" ] && fail "rejected hook still ran"
chmod 600 "$HOOK"
ctl reload >/dev/null
wait_log ERROR "" "why=not executable"
ctl ping >/dev/null || fail "daemon died on a bad hook"
ok "world-writable and non-executable hooks rejected with ERROR; daemon unaffected"

chmod 700 "$HOOK"
ctl reload >/dev/null
echo y >"$TREE/after-fix"
for i in $(seq 1 30); do grep -q after-fix "$OUT" 2>/dev/null && break; sleep 0.1; done
grep -q after-fix "$OUT" || fail "fixed hook not re-enabled by RELOAD"
ok "RELOAD re-validates: fixed hook runs again"
