#!/usr/bin/env bash
# Proves tools/check-release-layout.py fails when any of the files that name the
# release asset stops naming it cobalt-<version>.wuhb. Run from the repo root.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

# A copy of just the files under test, so a violation can be injected.
fresh() {
    rm -rf "$tmp/root"
    mkdir -p "$tmp/root/src/util" "$tmp/root/tools"
    cp "$here/src/util/version.h" "$tmp/root/src/util/"
    cp "$here/tools/make-hbas-listing.py" "$here/tools/check-hbas-listing.py" \
       "$here/tools/publish.sh" "$here/tools/submit-hbas.sh" "$tmp/root/tools/"
}
expect() { # expect pass|fail label
    local want=$1 label=$2
    if python3 "$here/tools/check-release-layout.py" "$tmp/root" >/dev/null 2>&1; then got=pass; else got=fail; fi
    [ "$got" = "$want" ] && echo "ok   selftest: $label ($want)" \
        || { echo "FAIL selftest: $label wanted $want, got $got"; bad=$((bad + 1)); }
}
bad=0

fresh
expect pass "the real tree"

# One mutation per rule: rename the asset in a single file and nothing else.
mutate() { # mutate <file> <sed-expression>
    fresh
    sed -i.bak "$2" "$tmp/root/$1"
}
mutate tools/make-hbas-listing.py 's|cobalt-%s\.wuhb|cobalt-%s-v2.wuhb|'
expect fail "generator naming a different asset"
mutate tools/check-hbas-listing.py 's|cobalt-%s\.wuhb|cobalt-%s-v2.wuhb|'
expect fail "listing check naming a different asset"
mutate tools/publish.sh 's|"dist/cobalt-\$new\.wuhb"|"dist/cobalt-v$new.wuhb"|g'
expect fail "publish.sh staging a different asset"
mutate tools/submit-hbas.sh 's|cobalt-\$version\.wuhb|cobalt-v$version.wuhb|g'
expect fail "submit-hbas.sh looking for a different asset"

# The release line itself, with the staging step left intact.
fresh
python3 - "$tmp/root/tools/publish.sh" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace('gh release create "v$new" "dist/cobalt-$new.wuhb"',
              'gh release create "v$new" "dist/cobalt.wuhb"')
open(p, "w").write(s)
PY
expect fail "a release that attaches a differently named file"

# A missing file must fail rather than pass quietly.
fresh
rm "$tmp/root/tools/submit-hbas.sh"
expect fail "a tool that is not there"

[ "$bad" -eq 0 ] && echo "all release-layout selftests passed" || exit 1
