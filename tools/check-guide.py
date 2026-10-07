#!/usr/bin/env python3
"""docs/guide.md must show only images that exist, and name every home entry.

    python3 tools/check-guide.py [guide.md]
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
guide = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs/guide.md")
text = open(guide).read()
errs = []
for m in re.finditer(r"!\[[^\]]*\]\(([^)]+)\)", text):
    p = os.path.join(os.path.dirname(guide), m.group(1))
    if not os.path.isfile(p):
        errs.append("image missing: %s" % m.group(1))
# The home menu is the guide's table of contents: every entry in app_home.c must be named.
app = open(os.path.join(ROOT, "src/app/app_home.c")).read()
block = app[app.index("menu_label(int index)"):app.index("menu_hint(int index)")]
labels = re.findall(r'case ACTION_\w+:\s+return (?:signed_in\(\) \? )?"([^"]+)"', block)
for label in labels:
    if label not in text and not (label == "Sign in" and "Sign in" in text):
        errs.append("guide never mentions the home entry %r" % label)
for e in errs:
    print("FAIL [guide] " + e)
if errs:
    sys.exit(1)
print("ok   [guide] %s" % os.path.relpath(guide, ROOT))
