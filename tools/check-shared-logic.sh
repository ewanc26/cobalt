#!/usr/bin/env bash
# Guards against re-growing logic that belongs in Wolfram (AGENTS.md, "Shared
# logic lives in Wolfram"). Run from the repository root.
#
#   tools/check-shared-logic.sh [src-dir]
#
# Each rule is a pattern that must not appear in src/ outside an allow-list. The
# allow-list shrinks as Wolfram takes things over; adding to it needs a reason
# in the commit.
set -u
src=${1:-src}
fails=0

# pattern | allowed file (or "-" for none) | what to use instead
rules=(
  'whole_word|is_word_byte|-|wf_mod_match_mute_words (muted-word matching is Wolfram'"'"'s)'
  'uk\.ewancroft\.oauth\.|-|wf_oauth_pair_run from wolfram/oauth_pairing.h (the pairing contract is Wolfram'"'"'s)'
  '0x428a2f98|-|wf_sha256_* from wolfram/update.h (SHA-256 is Wolfram'"'"'s)'
  'parse_semver|cmp_pre|-|wf_update_compare_versions from wolfram/update.h (version comparison is Wolfram'"'"'s)'
)

for rule in "${rules[@]}"; do
    IFS='|' read -r -a parts <<<"$rule"
    # The pattern may itself contain '|'; the last two fields are always allow and advice.
    n=${#parts[@]}
    advice=${parts[n-1]}
    allow=${parts[n-2]}
    pattern=$(IFS='|'; echo "${parts[*]:0:n-2}")
    while IFS= read -r hit; do
        file=${hit%%:*}
        [ "$allow" != "-" ] && [[ $file == *"$allow" ]] && continue
        echo "FAIL [shared-logic] $hit  -> use $advice"
        fails=$((fails + 1))
    done < <(grep -rnE "$pattern" "$src" --include='*.c' --include='*.h' 2>/dev/null)
done

[ "$fails" -eq 0 ] && echo "ok   [shared-logic] nothing re-grown in $src" || { echo "$fails shared-logic violation(s)."; exit 1; }
