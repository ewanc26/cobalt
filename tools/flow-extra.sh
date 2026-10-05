#!/usr/bin/env bash
# Cobalt's additions to the stack's flow checks (docs/workflow.md): a change to
# src/ must come with a CHANGELOG line and with a README, AGENTS.md or docs/
# change, or say why not in the description.
#
# Inputs (environment): PR_BODY, BASE_SHA, HEAD_SHA. CODE_PATHS is an extended
# regex of files that count as code (default ^src/).
set -u
PR_BODY=${PR_BODY-}
CODE_PATHS=${CODE_PATHS:-^src/}
fails=0

changed=$(git diff --name-only "${BASE_SHA:?}...${HEAD_SHA:?}" 2>/dev/null)
if ! grep -Eq "$CODE_PATHS" <<<"$changed"; then
    echo "flow-extra: no code changed; nothing to check"
    exit 0
fi

if ! grep -qx 'CHANGELOG.md' <<<"$changed" && ! grep -Eqi '^Changelog: none \(.+\)' <<<"$PR_BODY"; then
    echo "flow-extra: code changed but CHANGELOG.md did not; add a line under [Unreleased] or put 'Changelog: none (reason)' in the description" >&2
    fails=$((fails + 1))
fi
if ! grep -Eq '^(AGENTS\.md|README\.md|docs/)' <<<"$changed" && ! grep -Eqi '^Docs: none \(.+\)' <<<"$PR_BODY"; then
    echo "flow-extra: code changed but AGENTS.md, README.md and docs/ did not; update them or put 'Docs: none (reason)' in the description" >&2
    fails=$((fails + 1))
fi
[ "$fails" -eq 0 ] && echo "flow-extra: changelog and docs moved with the code"
exit "$fails"
