#!/usr/bin/env bash
# Print the update.json for a release to stdout.
#
#   tools/make-update-manifest.sh <version> <path-to-wuhb> [changelog-notes-file]
#
# The file name, size and SHA-256 come from the .wuhb itself, and the URL from
# the repository's release layout, so the manifest cannot disagree with the
# asset it describes. The contract is ewanc26/wolfram#106; src/update/update.c
# is its reader and refuses anything this does not produce.
set -euo pipefail
version=${1:?usage: make-update-manifest.sh <version> <wuhb> [notes-file]}
wuhb=${2:?usage: make-update-manifest.sh <version> <wuhb> [notes-file]}
notes_file=${3:-}
repo=${COBALT_REPO:-ewanc26/cobalt}

name=cobalt-$version.wuhb
size=$(wc -c <"$wuhb" | tr -d ' ')
if command -v sha256sum >/dev/null 2>&1; then
    sha=$(sha256sum "$wuhb" | cut -d' ' -f1)
else
    sha=$(shasum -a 256 "$wuhb" | cut -d' ' -f1)
fi

python3 - "$version" "$name" "$size" "$sha" "$repo" "$notes_file" <<'PY'
import json, sys
version, name, size, sha, repo, notes_file = sys.argv[1:7]
notes = ""
if notes_file:
    # The reader keeps 511 bytes; give it a clean cut, not a rejected manifest.
    notes = open(notes_file).read().strip().encode()[:480].decode("utf-8", "ignore")
print(json.dumps({
    "schema": 1,
    "app": "cobalt",
    "version": version,
    "notes": notes or None,
    "asset": {
        "name": name,
        "url": f"https://github.com/{repo}/releases/download/v{version}/{name}",
        "size": int(size),
        "sha256": sha,
    },
    "signature": None,
}, indent=2))
PY
