#!/usr/bin/env bash
# Proves tools/check-guide.py fails on a guide with a missing image or a missing home entry.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
bad=0
expect() { if python3 "$here/tools/check-guide.py" "$2" >/dev/null 2>&1; then got=pass; else got=fail; fi
  [ "$got" = "$1" ] && echo "ok   selftest: $3 ($1)" || { echo "FAIL selftest: $3 wanted $1, got $got"; bad=$((bad + 1)); }; }
mkdir -p "$tmp/screenshots"
cp "$here/docs/guide.md" "$tmp/guide.md"; cp "$here"/docs/screenshots/*.png "$tmp/screenshots/"
expect pass "$tmp/guide.md" "the real guide"
printf '\n![x](screenshots/nope.png)\n' >> "$tmp/guide.md"
expect fail "$tmp/guide.md" "an image that does not exist"
cp "$here/docs/guide.md" "$tmp/guide.md"
sed -i 's/Diagnostics/Dx/g' "$tmp/guide.md"
expect fail "$tmp/guide.md" "a home entry the guide forgot"
[ "$bad" -eq 0 ] && echo "all guide selftests passed" || exit 1
