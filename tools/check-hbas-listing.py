#!/usr/bin/env python3
"""Validate the folder tools/make-hbas-listing.py wrote.

    python3 tools/check-hbas-listing.py [dist/hbas] [--expect-version X.Y.Z]

Checks what the store needs and what I could verify from existing listings:
required keys, one `update` asset whose URL names this repository's release
for this version, every referenced file present and a real PNG, a 128x128 icon,
and sizes that keep the details and changelog readable. It cannot know the
store's own validation beyond that; docs/homebrew-app-store.md says which parts
are unverified.
"""
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def png_size(path):
    d = open(path, "rb").read(32)
    if d[:8] != b"\x89PNG\r\n\x1a\n" or d[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", d[16:24])


def check(out, expect):
    errs = []
    pkg = os.path.join(out, "packages", "Cobalt")
    try:
        meta = json.load(open(os.path.join(pkg, "pkgbuild.json")))
    except Exception as e:  # noqa: BLE001
        return ["pkgbuild.json: %s" % e]
    if meta.get("package") != "Cobalt":
        errs.append("package must be 'Cobalt'")
    info = meta.get("info", {})
    for k in ("title", "author", "category", "version", "url", "license", "description", "details"):
        if not isinstance(info.get(k), str) or not info[k].strip():
            errs.append("info.%s missing or empty" % k)
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?", info.get("version", "")):
        errs.append("info.version is not x.y.z")
    if expect and info.get("version") != expect:
        errs.append("info.version %s != expected %s" % (info.get("version"), expect))
    if len(info.get("description", "")) > 100:
        errs.append("info.description is over 100 characters")
    if not isinstance(meta.get("changelog"), str) or not meta["changelog"].strip():
        errs.append("changelog missing or empty")
    assets = meta.get("assets", [])
    updates = [x for x in assets if x.get("type") == "update"]
    if len(updates) != 1:
        errs.append("want exactly one update asset, found %d" % len(updates))
    for u in updates:
        want = "https://github.com/ewanc26/cobalt/releases/download/v%s/cobalt-%s.wuhb" % (
            info.get("version"), info.get("version"))
        if u.get("url") != want:
            errs.append("update url %r is not %r" % (u.get("url"), want))
        if u.get("dest") != "/wiiu/apps/cobalt.wuhb":
            errs.append("update dest must be /wiiu/apps/cobalt.wuhb (where the updater expects it)")
    types = [x.get("type") for x in assets]
    for t in ("icon", "banner"):
        if types.count(t) != 1:
            errs.append("want exactly one %s asset" % t)
    if "screenshot" not in types:
        errs.append("no screenshot assets (run the snapshot renderer first)")
    for x in assets:
        if x.get("type") == "update":
            continue
        fn = os.path.join(pkg, x.get("url", ""))
        if not os.path.isfile(fn):
            errs.append("%s: file missing" % x.get("url"))
            continue
        size = png_size(fn)
        if size is None:
            errs.append("%s: not a PNG" % x["url"])
        elif x["type"] == "icon" and size != (128, 128):
            errs.append("icon is %dx%d, expected 128x128" % size)
        elif x["type"] in ("banner", "screenshot") and (size[0] < 640 or size[1] < 360):
            errs.append("%s is %dx%d, too small" % (x["url"], size[0], size[1]))
    return errs


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    expect = None
    if "--expect-version" in sys.argv:
        expect = sys.argv[sys.argv.index("--expect-version") + 1]
        args = [a for a in args if a != expect]
    out = args[0] if args else os.path.join(ROOT, "dist/hbas")
    errs = check(out, expect)
    for e in errs:
        print("FAIL [hbas] " + e)
    if errs:
        sys.exit(1)
    print("ok   [hbas] %s" % out)


if __name__ == "__main__":
    main()
