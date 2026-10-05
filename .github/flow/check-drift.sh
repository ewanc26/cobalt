#!/usr/bin/env bash
# Drift check for the unified flow. Fails when the written flow, the files that
# enforce it and the canonical copy in the hosting repo have come apart.
#
#   check-drift.sh [canonical-checkout]
#
# The canonical checkout is a checkout of the repo that hosts the shared flow
# files (Cobalt today, Wolfram once it takes them over). When it is given, any
# shared file that differs from it fails. Without it only the local checks run.
set -u
canon=${1:-}
fails=0
fail() { echo "FAIL [drift] $1"; fails=$((fails + 1)); }
ok() { echo "ok   [drift] $1"; }

# Files the flow depends on. Missing one means the written flow is not enforced.
required=(
    docs/workflow.md
    .github/pull_request_template.md
    .github/flow/flow-check.sh
    .github/flow/check-drift.sh
    .github/flow/flow-selftest.sh
    .github/workflows/flow.yml
    .github/workflows/pr-flow.yml
    .github/workflows/ci.yml
    .github/workflows/release-check.yml
)
for f in "${required[@]}"; do
    [ -f "$f" ] && ok "$f present" || fail "$f is missing"
done

# Files shared byte for byte across the stack.
shared=(
    .github/flow/flow-check.sh
    .github/flow/check-drift.sh
    .github/flow/flow-selftest.sh
    .github/workflows/flow.yml
    .github/pull_request_template.md
)
if [ -n "$canon" ] && [ -d "$canon" ]; then
    for f in "${shared[@]}"; do
        if [ ! -f "$canon/$f" ]; then
            fail "$f is not in the canonical checkout"
        elif cmp -s "$f" "$canon/$f"; then
            ok "$f matches canonical"
        else
            fail "$f differs from the canonical copy; change it there first, then copy it here"
        fi
    done
else
    echo "note [drift] no canonical checkout given; skipped the byte comparison"
fi

# The written flow must name every check script and workflow it relies on.
for f in flow-check.sh check-drift.sh flow-selftest.sh flow.yml pull_request_template.md; do
    grep -q "$f" docs/workflow.md || fail "docs/workflow.md does not mention $f"
done
grep -q 'docs/workflow.md' AGENTS.md || fail "AGENTS.md does not point at docs/workflow.md"
grep -q 'docs/workflow.md' CONTRIBUTING.md || fail "CONTRIBUTING.md does not point at docs/workflow.md"

# CHANGELOG must start its history with an Unreleased section, or release.sh has
# nowhere to take notes from.
first=$(grep -m1 '^## ' CHANGELOG.md)
[ "$first" = "## [Unreleased]" ] && ok "CHANGELOG starts with [Unreleased]" || fail "CHANGELOG's first section is '$first', not [Unreleased]"

[ "$fails" -eq 0 ] || { echo "$fails drift check(s) failed."; exit 1; }
