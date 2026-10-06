#!/usr/bin/env python3
"""Write the screenshots docs/guide.md shows, from the host snapshot renderer.

    make -C tests snapshot
    python3 tools/make-guide-images.py [tests/build/snapshot]

The renderer draws the real screens on a PC from fixture data (an invented
account), not on a Wii U. This converts a fixed set of its TV frames to PNG in
docs/screenshots/. Re-run it when a screen changes visibly; `tools/check-guide.py`
only checks that every image the guide names exists.
"""
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("hbas", os.path.join(ROOT, "tools/make-hbas-listing.py"))
hbas = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hbas)

# guide image -> snapshot frame
FRAMES = {
    "home-signedin": "home-signedin-tv",
    "timeline": "timeline-2-tv",
    "thread": "thread-tv",
    "compose": "compose-tv",
    "notifications": "notifications-tv",
    "more-menu": "timeline-menu-tv",
    "feeds": "feeds-tv",
}

src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests/build/snapshot")
out = os.path.join(ROOT, "docs/screenshots")
os.makedirs(out, exist_ok=True)
for name, frame in FRAMES.items():
    w, h, rows = hbas.read_bmp(os.path.join(src, frame + ".bmp"))
    hbas.write_png(os.path.join(out, name + ".png"), w, h, rows)
    print("wrote docs/screenshots/%s.png (%dx%d)" % (name, w, h))
