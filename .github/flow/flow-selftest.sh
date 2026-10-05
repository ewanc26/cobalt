#!/usr/bin/env bash
# Proves each flow check fails on a deliberate violation, and passes on a clean
# change. A check that cannot fail is not a check.
set -u
here=$(cd "$(dirname "$0")" && pwd)
check="$here/flow-check.sh"
drift="$here/check-drift.sh"
bad=0

expect() { # expect <pass|fail> <label> <command...>
    local want=$1 label=$2
    shift 2
    if "$@" >/dev/null 2>&1; then got=pass; else got=fail; fi
    if [ "$got" = "$want" ]; then
        echo "ok   selftest: $label ($want)"
    else
        echo "FAIL selftest: $label wanted $want, got $got"
        bad=$((bad + 1))
    fi
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp" || exit 1
git init -q -b main . && git config user.email t@t && git config user.name t
mkdir src docs
echo a > src/a.c && echo "# c" > CHANGELOG.md && git add -A && git commit -qm "chore: seed"
git checkout -q -b feat/x
echo b > src/a.c && git commit -qam "feat(app): change a thing"
export BASE=main HEAD=HEAD

good_body=$'## Summary\nx\n\n## Verification\nhost: make test\n\nChangelog: none (selftest)\nDocs: none (selftest)'
export BODY=$good_body BRANCH=feat/x TITLE="feat(app): change a thing"

expect pass "clean change" bash "$check" all
BRANCH=Feature_X expect fail "branch name" bash "$check" branch
BRANCH=main expect fail "branch is main" bash "$check" branch
TITLE="Update stuff." expect fail "title format" bash "$check" title
TITLE="feat(app): Capitalised" expect fail "title capital" bash "$check" title
BODY="" expect fail "empty description" bash "$check" body
BODY=$'## Summary\nx\n\n## Verification\n<!-- what ran -->' expect fail "empty verification" bash "$check" body
BODY="no changelog or docs" expect fail "changelog opt-out missing" bash "$check" changelog
BODY="no changelog or docs" expect fail "docs opt-out missing" bash "$check" docs
echo "- note" >> CHANGELOG.md && echo "d" > docs/d.md && git add -A && git commit -qm "docs: note it"
BODY="plain" expect pass "changelog and docs touched" bash "$check" changelog
BODY="plain" expect pass "docs touched" bash "$check" docs
git commit -q --allow-empty -m "WIP stuff"
expect fail "commit subject" bash "$check" commits

# Drift: a scratch tree with the required files, then break one.
d=$(mktemp -d)
cp -r "$here/.." "$d/.github"
cd "$d" || exit 1
mkdir docs
printf 'flow-check.sh check-drift.sh flow-selftest.sh flow.yml pull_request_template.md\n' > docs/workflow.md
echo "docs/workflow.md" > AGENTS.md
echo "docs/workflow.md" > CONTRIBUTING.md
printf '# c\n\n## [Unreleased]\n' > CHANGELOG.md
touch .github/pull_request_template.md
for f in ci release-check; do touch ".github/workflows/$f.yml"; done
cp -r "$d" "$d.canon"
expect pass "drift, in step with canonical" bash "$drift" "$d.canon"
echo "# tampered" >> .github/flow/flow-check.sh
expect fail "drift, flow script tampered" bash "$drift" "$d.canon"
cp "$d.canon/.github/flow/flow-check.sh" .github/flow/flow-check.sh
echo "# changed" >> "$d.canon/.github/pull_request_template.md"
expect fail "drift, template out of step" bash "$drift" "$d.canon"
sed -i 's/docs\/workflow.md//' AGENTS.md
expect fail "drift, AGENTS.md lost the pointer" bash "$drift"
rm -rf "$d" "$d.canon"

[ "$bad" -eq 0 ] || { echo "$bad selftest(s) failed"; exit 1; }
echo "all flow selftests passed"
