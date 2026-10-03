#!/usr/bin/env bash
# Cut a release: tools/release.sh <major|minor|patch|x.y.z>
#
# 1. Branches release/<version> from a clean, up-to-date main.
# 2. Bumps src/util/version.h and the README, and turns the CHANGELOG's
#    [Unreleased] section into the new version.
# 3. Runs the host tests, opens a PR and (with --merge) squash-merges it.
# 4. Tags the merge commit, builds the .wuhb from it and publishes a GitHub
#    release with the CHANGELOG notes and the .wuhb attached.
#
# Needs: git, gh, python3, make, and a devkitPro install for the bundle.
# Pass --dry-run to stop after the checks, --merge to merge without waiting.
set -euo pipefail
cd "$(dirname "$0")/.."

bump=${1:?usage: tools/release.sh <major|minor|patch|x.y.z> [--merge] [--dry-run]}
shift || true
merge=0 dry=0
for a in "$@"; do
  case $a in --merge) merge=1 ;; --dry-run) dry=1 ;; *) echo "unknown flag $a" >&2; exit 2 ;; esac
done

[ "$(git rev-parse --abbrev-ref HEAD)" = main ] || { echo "run from main" >&2; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "working tree not clean" >&2; exit 1; }
git pull -q --ff-only origin main

current=$(sed -n 's/.*COBALT_VERSION "\(.*\)".*/\1/p' src/util/version.h)
case $bump in
  major|minor|patch)
    IFS=. read -r ma mi pa <<<"$current"
    case $bump in
      major) new="$((ma + 1)).0.0" ;;
      minor) new="$ma.$((mi + 1)).0" ;;
      patch) new="$ma.$mi.$((pa + 1))" ;;
    esac ;;
  *) new=$bump ;;
esac
[[ $new =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "bad version: $new" >&2; exit 1; }
! git rev-parse -q --verify "refs/tags/v$new" >/dev/null || { echo "v$new already exists" >&2; exit 1; }
grep -q '^## \[Unreleased\]' CHANGELOG.md || { echo "CHANGELOG has no [Unreleased] section" >&2; exit 1; }
echo "Releasing $current -> $new"

git checkout -q -b "release/$new"
sed -i.bak "s/COBALT_VERSION \"$current\"/COBALT_VERSION \"$new\"/" src/util/version.h
sed -i.bak "s/\*\*Version $current\*\*/**Version $new**/" README.md
rm -f src/util/version.h.bak README.md.bak
python3 - "$current" "$new" <<'PY'
import re, sys, datetime
cur, new = sys.argv[1:]
s = open("CHANGELOG.md").read()
body = s.split("## [Unreleased]", 1)[1].split("\n## [", 1)[0].strip()
if not body:
    sys.exit("the [Unreleased] section is empty: write the notes first")
today = datetime.date.today().isoformat()
s = s.replace("## [Unreleased]\n", f"## [Unreleased]\n\n## [{new}] - {today}\n", 1)
s = re.sub(r"\[Unreleased\]: .*", f"[Unreleased]: https://github.com/ewanc26/cobalt/compare/v{new}...HEAD\n[{new}]: https://github.com/ewanc26/cobalt/compare/v{cur}...v{new}", s, 1)
open("CHANGELOG.md", "w").write(s)
PY

make test
if [ $dry = 1 ]; then echo "dry run: leaving release/$new uncommitted"; exit 0; fi

git add src/util/version.h README.md CHANGELOG.md
git commit -q -m "Release $new"
git push -q -u origin "release/$new"
pr=$(gh pr create --title "Release $new" --body "Release $new. Notes are in CHANGELOG.md.")
if [ $merge = 0 ]; then
  echo "Review and merge $pr, then run: tools/publish.sh $new"; exit 0
fi
gh pr merge "$pr" --squash --delete-branch
git checkout -q main && git pull -q --ff-only origin main

tools/publish.sh "$new"
