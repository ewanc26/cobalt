#!/usr/bin/env bash
# Tag the current main as v<version>, build the .wuhb and publish the release.
# Run by tools/release.sh after the release PR is merged; run it by hand if
# you merged the PR yourself. Usage: tools/publish.sh <x.y.z>
set -euo pipefail
cd "$(dirname "$0")/.."
new=${1:?usage: tools/publish.sh <x.y.z>}

[ "$(git rev-parse --abbrev-ref HEAD)" = main ] || { echo "run from main" >&2; exit 1; }
git pull -q --ff-only origin main
have=$(sed -n 's/.*COBALT_VERSION "\(.*\)".*/\1/p' src/util/version.h)
[ "$have" = "$new" ] || { echo "main is at $have, not $new" >&2; exit 1; }

notes=$(python3 - "$new" <<'PY'
import re, sys
s = open("CHANGELOG.md").read()
m = re.search(r"^## \[%s\][^\n]*\n(.*?)(?=^## \[|^\[Unreleased\]:)" % re.escape(sys.argv[1]), s, re.S | re.M)
if not m:
    sys.exit("no CHANGELOG section for " + sys.argv[1])
print(m.group(1).strip())
PY
)

git tag -a "v$new" -m "Cobalt $new"
git push -q origin "v$new"
DEVKITPRO=${DEVKITPRO:-/opt/devkitpro} DEVKITPPC=${DEVKITPPC:-/opt/devkitpro/devkitPPC} make bundle
cp dist/wiiu/apps/cobalt.wuhb "dist/cobalt-$new.wuhb"
gh release create "v$new" "dist/cobalt-$new.wuhb" --title "Cobalt $new" --notes "$notes" --verify-tag
echo "Published v$new"
