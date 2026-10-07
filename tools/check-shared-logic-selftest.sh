#!/usr/bin/env bash
# The shared-logic guard must fail on each deliberate violation.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
bad=0
expect() { # expect pass|fail label
    if bash "$here/tools/check-shared-logic.sh" "$tmp/src" >/dev/null 2>&1; then got=pass; else got=fail; fi
    [ "$got" = "$1" ] && echo "ok   selftest: $2 ($1)" || { echo "FAIL selftest: $2 wanted $1, got $got"; bad=$((bad + 1)); }
}
mkdir -p "$tmp/src/atproto"
echo 'int x;' > "$tmp/src/atproto/a.c"
expect pass "clean tree"
echo 'static int is_word_byte(int c) { return c; }' > "$tmp/src/atproto/prefs.c"
expect fail "muted-word matcher grows back"
echo 'int x;' > "$tmp/src/atproto/prefs.c"
echo 'const char *m = "uk.ewancroft.oauth.begin";' > "$tmp/src/atproto/session.c"
expect fail "pairing method back in session.c"
echo 'const char *m = "uk.ewancroft.oauth.poll";' > "$tmp/src/atproto/other.c"
expect fail "pairing method elsewhere"
echo 'static const unsigned K[1] = { 0x428a2f98 };' > "$tmp/src/atproto/sha.c"
expect fail "SHA-256 grows back"
echo 'int x;' > "$tmp/src/atproto/sha.c"
echo 'static int parse_semver(const char *s) { return 0; }' > "$tmp/src/atproto/ver.c"
expect fail "semver comparison grows back"
echo 'int x;' > "$tmp/src/atproto/ver.c"
echo 'static long days_from_civil(long y) { return y; }' > "$tmp/src/atproto/t.c"
expect fail "civil-date conversion grows back"
echo 'int x;' > "$tmp/src/atproto/t.c"
echo 'const char *m = "image/jpeg";' > "$tmp/src/atproto/mime.c"
expect fail "attachment MIME mapping grows back"
[ "$bad" -eq 0 ] && echo "all shared-logic selftests passed" || exit 1
