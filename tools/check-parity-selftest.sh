#!/usr/bin/env bash
# Proves tools/check-parity.sh fails on each deliberate violation.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
bad=0
run() { # run <pass|fail> <label> <parity> <readme> <agents>
    if bash "$here/tools/check-parity.sh" "$3" "$4" "$5" >/dev/null 2>&1; then got=pass; else got=fail; fi
    [ "$got" = "$1" ] && echo "ok   selftest: $2 ($1)" || { echo "FAIL selftest: $2 wanted $1, got $got"; bad=$((bad + 1)); }
}
P=$here/docs/PARITY.md R=$here/README.md A=$here/AGENTS.md
run pass "real files" "$P" "$R" "$A"

sed 's/| App password | implemented/| App password | done/' "$P" > "$tmp/p1"
run fail "unknown state word" "$tmp/p1" "$R" "$A"
sed 's/issue platinum#38/issue platinum/' "$P" > "$tmp/p2"
run fail "issue cell with no reference" "$tmp/p2" "$R" "$A"
sed 's/^| OAuth | implemented/| OAuth | issue #1/' "$P" > "$tmp/p3"
run fail "OAuth marked as a gap" "$tmp/p3" "$R" "$A"
sed 's/^- \*\*Video and GIFs\*\*/- **OAuth sign-in** is not planned.\n- **Video and GIFs**/' "$R" > "$tmp/r1"
run fail "README lists OAuth as not planned" "$P" "$tmp/r1" "$A"
sed 's/App-password sign-in with an on-screen keyboard/Sign-in/' "$R" > "$tmp/r2"
run fail "README drops the app-password row" "$P" "$tmp/r2" "$A"
{ cat "$A"; echo '| OAuth sign-in | §7 — nowhere to host a redirect target |'; } > "$tmp/a1"
run fail "AGENTS.md says OAuth is not viable" "$P" "$R" "$tmp/a1"

[ "$bad" -eq 0 ] && echo "all parity selftests passed" || { echo "$bad parity selftest(s) failed"; exit 1; }
