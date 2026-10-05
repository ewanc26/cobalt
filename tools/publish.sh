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

# Release gate: main's CI must be finished and green on this exact commit, the
# same condition the release-check workflow enforces on the tag.
sha=$(git rev-parse HEAD)
runs=$(gh api "repos/{owner}/{repo}/commits/$sha/check-runs?per_page=100" \
  --jq '[.check_runs[] | select(.name=="host-tests" or .name=="wuhb")] | map(.status + ":" + (.conclusion // "")) | join(" ")')
[ "$(wc -w <<<"$runs")" -ge 2 ] || { echo "no host-tests/wuhb run on $sha yet; wait for CI" >&2; exit 1; }
[ "$(tr ' ' '\n' <<<"$runs" | grep -vc '^completed:success$')" -eq 0 ] || { echo "CI is not green on $sha ($runs)" >&2; exit 1; }

git tag -a "v$new" -m "Cobalt $new"
git push -q origin "v$new"
DEVKITPRO=${DEVKITPRO:-/opt/devkitpro} DEVKITPPC=${DEVKITPPC:-/opt/devkitpro/devkitPPC} make bundle
cp dist/wiiu/apps/cobalt.wuhb "dist/cobalt-$new.wuhb"

# Belt and braces: `make bundle` refuses to run without Wolfram linked, but a
# release must never ship a build whose whole protocol layer is compiled out.
# The WUHB compresses the RPX internally, so verify the link output instead:
# a Wolfram-linked build has libwolfram.a in its linker map, a Wolfram-free
# one does not.
if ! grep -q "libwolfram.a" build/cobalt.map; then
  echo "refusing to publish: libwolfram.a absent from build/cobalt.map — the protocol layer is missing" >&2
  exit 1
fi
# The updater's inputs: the checksum next to the build and the manifest Cobalt
# reads. Written from the .wuhb that is about to be attached, never from a
# different build, and checked against the reader's own rules before upload.
( cd dist && { sha256sum "cobalt-$new.wuhb" 2>/dev/null || shasum -a 256 "cobalt-$new.wuhb"; } >"cobalt-$new.wuhb.sha256" )
printf '%s\n' "$notes" >dist/notes.txt
tools/make-update-manifest.sh "$new" "dist/cobalt-$new.wuhb" dist/notes.txt >dist/update.json
python3 -c 'import json,sys; m=json.load(open("dist/update.json")); assert m["asset"]["size"]>0 and len(m["asset"]["sha256"])==64' \
  || { echo "refusing to publish: update.json is malformed" >&2; exit 1; }
gh release create "v$new" "dist/cobalt-$new.wuhb" "dist/cobalt-$new.wuhb.sha256" dist/update.json --title "Cobalt $new" --notes "$notes" --verify-tag
echo "Published v$new"
