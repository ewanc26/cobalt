#!/usr/bin/env python3
"""Build Cobalt's Homebrew App Store (fortheusers/wiiu-hbas-repo) package folder.

    python3 tools/make-hbas-listing.py [--version X.Y.Z] [--out dist/hbas]
                                       [--screenshots tests/build/snapshot]

The store builds its packages from one folder per app under `packages/`, holding
`pkgbuild.json`, `icon.png`, a banner `screen.png` and `screen-N.png`
screenshots (layout and field names read from the live CafXplorer package; see
docs/homebrew-app-store.md for what was and was not verifiable). This writes that
folder from what the repository already has, so a listing or an update is never
typed by hand:

  - the metadata and changelog from CHANGELOG.md and the README,
  - the .wuhb URL from the GitHub release layout `tools/publish.sh` produces,
  - the icon from assets/icon.png, the banner from assets/tv_splash.png,
  - screenshots from the host snapshot renderer (BMP to PNG, stdlib only).

Pure stdlib, no network. `tools/check-hbas-listing.py` validates the result.
"""
import argparse
import json
import os
import re
import shutil
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "ewanc26/cobalt"
PACKAGE = "Cobalt"
DEST = "/wiiu/apps/cobalt.wuhb"
# Which snapshots make the best screenshots, in order. TV frames, 1280x720.
SHOTS = ["home-signedin-tv", "timeline-2-tv", "thread-tv", "notifications-tv",
         "profile-tv", "feeds-tv", "search-tv"]

DESCRIPTION = "A Bluesky client for the Wii U"
DETAILS = (
    "A native Bluesky / AT Protocol client for the Nintendo Wii U, built for Aroma.\n"
    "\n"
    "Read your home timeline, threads, profiles, notifications and custom feeds; "
    "post, reply, quote and attach images; like and repost; search; mute and block. "
    "TV and GamePad are both first-class, including Off-TV Play.\n"
    "\n"
    "Sign in with an app password, or through a hosted Wolfram OAuth node so your "
    "password never touches the console.\n"
    "\n"
    "On first run, Cobalt asks you to scribble on the GamePad to make a private "
    "random seed (nothing can go online without one).\n"
    "\n"
    "Cobalt can check this GitHub project for updates from its Updates screen. "
    "It only downloads when you press A."
)


def read_version():
    s = open(os.path.join(ROOT, "src/util/version.h")).read()
    return re.search(r'COBALT_VERSION "([^"]+)"', s).group(1)


def changelog_text(version):
    s = open(os.path.join(ROOT, "CHANGELOG.md")).read()
    out = []
    for m in re.finditer(r"^## \[([^\]]+)\][^\n]*\n(.*?)(?=^## \[|\Z)", s, re.S | re.M):
        name, body = m.group(1), m.group(2).strip()
        if name == "Unreleased":
            continue
        # Plain text for a small screen: drop link syntax and headings' hashes.
        body = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", body)
        body = re.sub(r"^#+\s*", "", body, flags=re.M)
        out.append("v%s\n%s" % (name, body))
        if len(out) == 5:
            break
    text = "\n\n".join(out)
    return text[:6000]


def read_bmp(path):
    d = open(path, "rb").read()
    if d[:2] != b"BM":
        raise SystemExit("%s is not a BMP" % path)
    off = struct.unpack("<I", d[10:14])[0]
    w, h, planes, bpp = struct.unpack("<iiHH", d[18:30])
    if bpp not in (24, 32) or struct.unpack("<I", d[30:34])[0] not in (0, 3):
        raise SystemExit("%s: unsupported BMP (%d bpp)" % (path, bpp))
    flip = h > 0
    h = abs(h)
    bpr = ((w * bpp // 8) + 3) & ~3
    rows = []
    for y in range(h):
        sy = h - 1 - y if flip else y
        row = d[off + sy * bpr: off + sy * bpr + w * bpp // 8]
        px = bytearray()
        step = bpp // 8
        for x in range(w):
            b, g, r = row[x * step], row[x * step + 1], row[x * step + 2]
            px += bytes((r, g, b))
        rows.append(bytes(px))
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default=read_version())
    ap.add_argument("--out", default=os.path.join(ROOT, "dist/hbas"))
    ap.add_argument("--screenshots", default=os.path.join(ROOT, "tests/build/snapshot"))
    a = ap.parse_args()

    pkg = os.path.join(a.out, "packages", PACKAGE)
    shutil.rmtree(pkg, ignore_errors=True)
    os.makedirs(pkg)

    shutil.copy(os.path.join(ROOT, "assets/icon.png"), os.path.join(pkg, "icon.png"))
    shutil.copy(os.path.join(ROOT, "assets/tv_splash.png"), os.path.join(pkg, "screen.png"))

    shots = []
    seen = set()
    for name in SHOTS:
        src = os.path.join(a.screenshots, name + ".bmp")
        if not os.path.exists(src):
            continue
        w, h, rows = read_bmp(src)
        digest = zlib.crc32(b"".join(rows))
        if digest in seen:
            continue   # two screens that rendered identically add nothing
        seen.add(digest)
        fn = "screen-%d.png" % len(shots)
        write_png(os.path.join(pkg, fn), w, h, rows)
        shots.append(fn)

    url = "https://github.com/%s/releases/download/v%s/cobalt-%s.wuhb" % (REPO, a.version, a.version)
    assets = [{"url": url, "type": "update", "dest": DEST},
              {"type": "icon", "url": "icon.png"},
              {"type": "banner", "url": "screen.png"}]
    assets += [{"type": "screenshot", "url": fn} for fn in shots]

    meta = {
        "package": PACKAGE,
        "info": {
            "title": "Cobalt",
            "author": "Ewan Croft",
            "category": "tool",
            "version": a.version,
            "url": "https://github.com/%s" % REPO,
            "license": "AGPL-3.0",
            "description": DESCRIPTION,
            "details": DETAILS,
        },
        "assets": assets,
        "changelog": changelog_text(a.version),
    }
    with open(os.path.join(pkg, "pkgbuild.json"), "w") as f:
        json.dump(meta, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print("wrote %s (%d screenshots)" % (pkg, len(shots)))


if __name__ == "__main__":
    main()
