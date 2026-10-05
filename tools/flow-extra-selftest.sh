#!/usr/bin/env bash
# Proves tools/flow-extra.sh fails on each deliberate violation.
set -u
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
bad=0
cd "$tmp" || exit 1
git init -q -b main . && git config user.email t@t && git config user.name t
mkdir src docs && echo a > src/a.c && echo "# c" > CHANGELOG.md && git add -A && git commit -qm "chore: seed"
base=$(git rev-parse HEAD)
echo b > src/a.c && git commit -qam "feat(app): change a thing"
code=$(git rev-parse HEAD)
echo n >> CHANGELOG.md && git commit -qam "docs: changelog"
withlog=$(git rev-parse HEAD)
echo d > docs/d.md && git add -A && git commit -qm "docs: a doc"
withboth=$(git rev-parse HEAD)

expect() { # expect pass|fail label head body
    if PR_BODY=$4 BASE_SHA=$base HEAD_SHA=$3 bash "$here/flow-extra.sh" >/dev/null 2>&1; then got=pass; else got=fail; fi
    [ "$got" = "$1" ] && echo "ok   selftest: $2 ($1)" || { echo "FAIL selftest: $2 wanted $1, got $got"; bad=$((bad + 1)); }
}
expect fail "code with no changelog or docs" "$code" ""
expect fail "changelog but no docs" "$withlog" ""
expect pass "changelog and docs" "$withboth" ""
expect pass "both opted out with a reason" "$code" $'Changelog: none (internal)\nDocs: none (internal)'
expect fail "opt-out without a reason" "$code" $'Changelog: none ()\nDocs: none ()'
[ "$bad" -eq 0 ] && echo "all flow-extra selftests passed" || exit 1
