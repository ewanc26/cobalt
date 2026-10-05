#!/usr/bin/env bash
# Keeps docs/PARITY.md, README.md and AGENTS.md telling the same story about
# what Cobalt does. Run from the repository root.
#
#   tools/check-parity.sh [parity-file] [readme] [agents]
set -u
parity=${1:-docs/PARITY.md}
readme=${2:-README.md}
agents=${3:-AGENTS.md}
fails=0
fail() { echo "FAIL [parity] $1"; fails=$((fails + 1)); }

state_re='^(implemented|partial|issue|declined|impossible|n/a)\b'
ref_re='(#[0-9]+|[a-z]+#[0-9]+)'

rows=0
trim() { sed 's/^[[:space:]]*//; s/[[:space:]]*$//' <<<"$1"; }
wide=0
while IFS='|' read -r _ name c i p _; do
    name=$(trim "$name"); c=$(trim "$c"); i=$(trim "$i"); p=$(trim "$p")
    case $name in
        Feature|Flow) wide=1; continue ;;
        Item) wide=0; continue ;;
        ---*|"") continue ;;
    esac
    cells=("$c")
    [ "$wide" -eq 1 ] && cells=("$c" "$i" "$p")
    for cell in "${cells[@]}"; do
        rows=$((rows + 1))
        if ! [[ $cell =~ $state_re ]]; then
            fail "'$name': cell '${cell:0:40}' does not start with a known state"
            continue
        fi
        case $cell in
            issue*|partial*) [[ $cell =~ $ref_re ]] || fail "'$name': '${cell:0:40}' names no issue" ;;
        esac
    done
done < <(grep '^|' "$parity")
[ "$rows" -gt 0 ] || fail "no table cells found in $parity"

# A row the README lists as implemented must not be an unqualified gap here, and
# one this page declines must not be listed as implemented in the README.
cobalt_state() { grep -m1 "^| $1 |" "$parity" | awk -F'|' '{gsub(/^ +| +$/,"",$3); print $3}'; }
for feature in "OAuth" "App password"; do
    s=$(cobalt_state "$feature")
    [[ $s == implemented* ]] || fail "$feature should be implemented in $parity (code has it), found '$s'"
done

if sed -n '/^### Deliberately not planned/,/^## /p' "$readme" | grep -qi "oauth"; then
    fail "$readme lists OAuth as not planned, but $parity says it is implemented"
fi
grep -qi "app-password sign-in" "$readme" && grep -qi "oauth" "$readme" \
    || fail "$readme must mention both app-password and OAuth sign-in"
if grep -q "| OAuth sign-in | §7" "$agents"; then
    fail "$agents still lists OAuth as not viable"
fi

[ "$fails" -eq 0 ] || { echo "$fails parity check(s) failed."; exit 1; }
echo "ok   [parity] $parity, $readme and $agents agree"
