#!/usr/bin/env bash
# Proves tools/check-hbas-listing.py fails on each deliberate violation.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
bad=0
fresh() { rm -rf "$tmp/out"; python3 "$here/tools/make-hbas-listing.py" --out "$tmp/out" --screenshots "$1" >/dev/null || { echo "FAIL selftest: generator failed"; exit 1; }; }
expect() { # expect pass|fail label [args...]
    local want=$1 label=$2; shift 2
    if python3 "$here/tools/check-hbas-listing.py" "$tmp/out" "$@" >/dev/null 2>&1; then got=pass; else got=fail; fi
    [ "$got" = "$want" ] && echo "ok   selftest: $label ($want)" || { echo "FAIL selftest: $label wanted $want, got $got"; bad=$((bad + 1)); }
}
# A synthetic 1280x720 snapshot so the test needs no renderer.
mkdir -p "$tmp/snap"
python3 - "$tmp/snap/home-signedin-tv.bmp" <<'PY'
import struct, sys
w, h = 1280, 720
data = b"\x20\x40\x80\xff" * w * h
dib = struct.pack("<IiiHHIIiiII", 108, w, h, 1, 32, 3, len(data), 0, 0, 0, 0) + b"\x00" * (108 - 40)
hdr = b"BM" + struct.pack("<IHHI", 14 + 108 + len(data), 0, 0, 14 + 108)
open(sys.argv[1], "wb").write(hdr + dib + data)
PY
fresh "$tmp/snap"
expect pass "a fresh listing"
expect fail "wrong expected version" --expect-version 0.0.1
cp -r "$tmp/out" "$tmp/good"
mut() { python3 - "$tmp/out/packages/Cobalt/pkgbuild.json" "$1" <<'PY'
import json, sys
p, what = sys.argv[1:]
m = json.load(open(p))
if what == "url": [a.update(url="https://example.com/x.wuhb") for a in m["assets"] if a["type"] == "update"]
if what == "dest": [a.update(dest="/wiiu/apps/other.wuhb") for a in m["assets"] if a["type"] == "update"]
if what == "nover": m["info"]["version"] = "latest"
if what == "nolog": m["changelog"] = ""
if what == "longdesc": m["info"]["description"] = "x" * 101
if what == "noshot": m["assets"] = [a for a in m["assets"] if a["type"] != "screenshot"]
json.dump(m, open(p, "w"))
PY
}
for w in url dest nover nolog longdesc noshot; do rm -rf "$tmp/out"; cp -r "$tmp/good" "$tmp/out"; mut $w; expect fail "listing with bad $w"; done
rm -rf "$tmp/out"; cp -r "$tmp/good" "$tmp/out"; rm "$tmp/out/packages/Cobalt/icon.png"; expect fail "missing icon file"
rm -rf "$tmp/out"; cp -r "$tmp/good" "$tmp/out"; echo notpng > "$tmp/out/packages/Cobalt/screen.png"; expect fail "banner that is not a PNG"
[ "$bad" -eq 0 ] && echo "all hbas selftests passed" || exit 1
