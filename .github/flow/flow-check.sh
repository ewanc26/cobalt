#!/usr/bin/env bash
# Unified-flow checks shared by the Wolfram stack. Hostable: it depends on bash
# and git only, and every input arrives by environment so it can be run (and
# deliberately broken) locally. See docs/workflow.md for the rules it enforces.
#
#   flow-check.sh [branch|title|body|commits|changelog|docs|all]
#
# Inputs: BRANCH, TITLE, BODY, BASE (commit-ish the PR targets), HEAD
# (commit-ish of the PR tip, default HEAD). CODE_PATHS is an extended regex for
# files that count as code (default ^src/).
set -u

mode=${1:-all}
BRANCH=${BRANCH:-}
TITLE=${TITLE:-}
BODY=${BODY:-}
BASE=${BASE:-origin/main}
HEAD_REF=${HEAD:-HEAD}
CODE_PATHS=${CODE_PATHS:-^src/}

types='feat|fix|docs|test|refactor|style|build|chore|ci|perf'
subject_re="^(${types})(\\([a-z0-9._-]+\\))?: [^A-Z ].*[^.]$"
branch_re="^((${types}|release)/[a-z0-9._-]+|claude/[A-Za-z0-9._-]+)$"

fails=0
fail() { echo "FAIL [$1] $2"; fails=$((fails + 1)); }
ok() { echo "ok   [$1] $2"; }

check_branch() {
    if [[ $BRANCH =~ $branch_re ]]; then
        ok branch "$BRANCH"
    else
        fail branch "'$BRANCH' is not <type>/<kebab-description> (types: ${types}, release; or claude/<name>)"
    fi
}

check_title() {
    if [[ $TITLE =~ $subject_re ]] && [ ${#TITLE} -le 72 ]; then
        ok title "$TITLE"
    elif [[ $BRANCH == release/* && $TITLE =~ ^Release\ [0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        ok title "$TITLE (release)"
    else
        fail title "'$TITLE' must be 'type(scope): lowercase imperative description', no full stop, at most 72 characters"
    fi
}

check_body() {
    if [[ $BRANCH == release/* ]]; then
        ok body "release PR, template not required"
        return
    fi
    local verification
    if ! grep -q '^## Summary' <<<"$BODY"; then
        fail body "PR description has no '## Summary' section (use .github/pull_request_template.md)"
    else
        ok body "has Summary"
    fi
    verification=$(awk '/^## Verification/{f=1;next} /^## /{f=0} f' <<<"$BODY" | sed 's/<!--.*-->//' | tr -d '[:space:]')
    if [ -z "$verification" ]; then
        fail body "'## Verification' is missing or empty: state what ran, on host, emulator or hardware"
    else
        ok body "has Verification"
    fi
}

commit_subjects() { git log --no-merges --format='%H %s' "${BASE}..${HEAD_REF}" 2>/dev/null; }

check_commits() {
    local n=0 line sha subj
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        n=$((n + 1))
        sha=${line%% *}
        subj=${line#* }
        if [[ $subj =~ $subject_re ]] && [ ${#subj} -le 72 ]; then
            ok commit "${sha:0:8} $subj"
        else
            fail commit "${sha:0:8} '$subj' is not 'type(scope): lowercase imperative description'"
        fi
    done < <(commit_subjects)
    [ "$n" -gt 0 ] || ok commit "no non-merge commits to check"
}

changed_files() { git diff --name-only "${BASE}...${HEAD_REF}" 2>/dev/null; }

touches_code() { changed_files | grep -Eq "$CODE_PATHS"; }

check_changelog() {
    if ! touches_code; then ok changelog "no code changed"; return; fi
    if changed_files | grep -qx 'CHANGELOG.md'; then
        ok changelog "CHANGELOG.md updated"
    elif grep -Eqi '^Changelog: none \(.+\)' <<<"$BODY"; then
        ok changelog "opted out in the description"
    else
        fail changelog "code changed but CHANGELOG.md did not; add a note under [Unreleased] or put 'Changelog: none (reason)' in the description"
    fi
}

check_docs() {
    if ! touches_code; then ok docs "no code changed"; return; fi
    if changed_files | grep -Eq '^(AGENTS\.md|README\.md|docs/)'; then
        ok docs "AGENTS.md, README or docs/ updated"
    elif grep -Eqi '^Docs: none \(.+\)' <<<"$BODY"; then
        ok docs "opted out in the description"
    else
        fail docs "code changed but AGENTS.md, README.md and docs/ did not; update them or put 'Docs: none (reason)' in the description"
    fi
}

case $mode in
    branch) check_branch ;;
    title) check_title ;;
    body) check_body ;;
    commits) check_commits ;;
    changelog) check_changelog ;;
    docs) check_docs ;;
    all) check_branch; check_title; check_body; check_commits; check_changelog; check_docs ;;
    *) echo "unknown check: $mode" >&2; exit 2 ;;
esac

[ "$fails" -eq 0 ] || { echo "$fails flow check(s) failed. See docs/workflow.md."; exit 1; }
