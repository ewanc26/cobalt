#!/usr/bin/env python3
"""Keep the release asset's name in step with the store listing.

    python3 tools/check-release-layout.py [root]

The Homebrew App Store package for Cobalt downloads

    https://github.com/ewanc26/cobalt/releases/download/v<version>/cobalt-<version>.wuhb

from a GitHub release, and the store's own automation rewrites that URL from
each new release (docs/homebrew-app-store.md). So a release whose `.wuhb` is
named anything else installs nothing: `hb-appstore` asks for a file that is not
there, and nothing here would have failed first. `tools/check-hbas-listing.py`
does not catch it either, because it validates the listing the generator wrote
from the version alone, not the file `tools/publish.sh` actually attaches.

Four files have to agree on the name: the listing generator writes the URL, the
listing check enforces it, `tools/publish.sh` attaches the file, and
`tools/submit-hbas.sh` looks for it on the release. This fails when one of them
stops naming it `cobalt-<version>.wuhb`.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# file | pattern it must still contain | what it is for
RULES = (
    ("tools/make-hbas-listing.py", r"releases/download/v%s/cobalt-%s\.wuhb",
     "the listing generator writes the store's download URL"),
    ("tools/check-hbas-listing.py", r"releases/download/v%s/cobalt-%s\.wuhb",
     "the listing check enforces that URL"),
    ("tools/publish.sh", r'"dist/cobalt-\$new\.wuhb"',
     "publish.sh stages the .wuhb under the name the store asks for"),
    ("tools/publish.sh", r'"dist/cobalt-\$new\.wuhb\.sha256"',
     "publish.sh publishes the checksum beside it"),
    ("tools/submit-hbas.sh", r'cobalt-\$version\.wuhb',
     "submit-hbas.sh looks for that asset on the release"),
)


def check(root):
    errs = []
    version_h = os.path.join(root, "src", "util", "version.h")
    try:
        m = re.search(r'COBALT_VERSION "([^"]+)"', open(version_h, encoding="utf-8").read())
    except OSError as e:
        return ["%s: %s" % (version_h, e)]
    if not m:
        return ["no COBALT_VERSION in %s" % version_h]
    version = m.group(1)
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?", version):
        errs.append("COBALT_VERSION is not x.y.z: %r" % version)

    for path, pattern, what in RULES:
        full = os.path.join(root, path)
        try:
            text = open(full, encoding="utf-8").read()
        except OSError as e:
            errs.append("%s: %s" % (path, e))
            continue
        if not re.search(pattern, text):
            errs.append("%s no longer names the asset cobalt-<version>.wuhb (%s): "
                        "the store would 404, so update this rule and the store "
                        "listing together" % (path, what))

    # The release itself, not just the staging step: gh release create has to
    # attach that exact file, or the asset the store asks for never exists.
    publish = os.path.join(root, "tools", "publish.sh")
    try:
        lines = [l for l in open(publish, encoding="utf-8").read().splitlines()
                 if "gh release create" in l]
    except OSError as e:
        lines = []
        errs.append("%s: %s" % ("tools/publish.sh", e))
    if not lines:
        errs.append("tools/publish.sh has no `gh release create` line to check")
    else:
        for l in lines:
            if not re.search(r'"dist/cobalt-\$new\.wuhb"', l):
                errs.append("the release does not attach cobalt-<version>.wuhb: %s" % l.strip())

    return errs


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    root = args[0] if args else ROOT
    errs = check(root)
    for e in errs:
        print("FAIL [release-layout] " + e)
    if errs:
        sys.exit(1)
    print("ok   [release-layout] the store's asset name is cobalt-<version>.wuhb everywhere")


if __name__ == "__main__":
    main()
