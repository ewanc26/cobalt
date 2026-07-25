#!/usr/bin/env python3
"""Generate Cobalt's WUHB artwork (icon + TV/DRC splash screens).

The assets are produced procedurally rather than checked in as opaque binaries
so the palette stays in one place and can be regenerated after a design change.
Pure stdlib on purpose: the Wii U toolchain box is not guaranteed to have
Pillow or ImageMagick available.

    python3 tools/gen_assets.py

Sizes are dictated by wuhbtool: 128x128 icon, 1280x720 TV splash, 854x480 DRC
splash. See AGENTS.md section 5 for the palette rationale (Wii U menu blues and
whites rather than Bluesky's own web branding).
"""

import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(os.path.dirname(HERE), "assets")

ICON_SIZE = 128
TV_SIZE = (1280, 720)
DRC_SIZE = (854, 480)

# Cobalt's palette: deep cobalt blue through to the Wii U menu's lighter sky
# blue, with white for the mark itself.
DEEP = (0x0B, 0x2E, 0x5C)
MID = (0x1C, 0x5A, 0xA8)
BRIGHT = (0x3E, 0x8E, 0xDE)
WHITE = (0xFF, 0xFF, 0xFF)


def lerp(a, b, t):
    return a + (b - a) * t


def mix(c1, c2, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(lerp(c1[i], c2[i], t))) for i in range(3))


def blend(dst, src, alpha):
    """Composite src over dst with the given 0..1 coverage."""
    alpha = max(0.0, min(1.0, alpha))
    return tuple(int(round(lerp(dst[i], src[i], alpha))) for i in range(3))


class Image:
    def __init__(self, width, height, fill=(0, 0, 0)):
        self.width = width
        self.height = height
        self.pixels = [[fill for _ in range(width)] for _ in range(height)]

    def put(self, x, y, colour, alpha=1.0):
        if 0 <= x < self.width and 0 <= y < self.height and alpha > 0.0:
            self.pixels[y][x] = blend(self.pixels[y][x], colour, alpha)

    def write_png(self, path):
        raw = bytearray()
        for row in self.pixels:
            raw.append(0)  # filter type 0 (None)
            for r, g, b in row:
                raw += bytes((r, g, b))

        def chunk(tag, data):
            out = struct.pack(">I", len(data)) + tag + data
            return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        header = struct.pack(">IIBBBBB", self.width, self.height, 8, 2, 0, 0, 0)
        png = b"\x89PNG\r\n\x1a\n"
        png += chunk(b"IHDR", header)
        png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        png += chunk(b"IEND", b"")

        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(png)
        print("wrote %s (%dx%d, %d bytes)" % (path, self.width, self.height, len(png)))


def vertical_gradient(image, top, bottom):
    for y in range(image.height):
        colour = mix(top, bottom, y / max(1, image.height - 1))
        for x in range(image.width):
            image.pixels[y][x] = colour


def glass_highlight(image, strength=0.22):
    """A soft elliptical sheen across the top, echoing the Wii U menu tiles."""
    cx = image.width * 0.5
    cy = -image.height * 0.25
    rx = image.width * 0.95
    ry = image.height * 0.85
    for y in range(image.height):
        for x in range(image.width):
            dx = (x + 0.5 - cx) / rx
            dy = (y + 0.5 - cy) / ry
            d = dx * dx + dy * dy
            if d < 1.0:
                image.put(x, y, WHITE, strength * (1.0 - d) ** 1.5)


def rounded_rect_mask(width, height, radius, x, y):
    """Antialiased coverage of a rounded rectangle at pixel centre (x, y)."""
    px, py = x + 0.5, y + 0.5
    # Distance outside the rounded rect, negative inside.
    dx = max(radius - px, px - (width - radius), 0.0)
    dy = max(radius - py, py - (height - radius), 0.0)
    if dx == 0.0 and dy == 0.0:
        return 1.0
    dist = math.hypot(dx, dy)
    return max(0.0, min(1.0, radius - dist + 0.5))


def draw_ring_c(image, cx, cy, outer, thickness, colour, gap_deg=(-38.0, 38.0)):
    """Draw a ring with a wedge removed — Cobalt's 'C' mark.

    Angles are measured with 0 degrees pointing right (+x) and increasing
    anticlockwise, so the default gap opens toward the right edge.
    """
    inner = outer - thickness
    lo, hi = gap_deg
    y0 = max(0, int(cy - outer) - 2)
    y1 = min(image.height, int(cy + outer) + 3)
    x0 = max(0, int(cx - outer) - 2)
    x1 = min(image.width, int(cx + outer) + 3)

    for y in range(y0, y1):
        for x in range(x0, x1):
            dx = x + 0.5 - cx
            dy = cy - (y + 0.5)
            dist = math.hypot(dx, dy)
            # Antialiased annulus coverage.
            cover = min(outer - dist + 0.5, dist - inner + 0.5)
            cover = max(0.0, min(1.0, cover))
            if cover <= 0.0:
                continue
            angle = math.degrees(math.atan2(dy, dx))
            # Signed angular distance into the gap: positive inside the wedge
            # that gets cut away, negative on the solid part of the ring.
            if lo <= angle <= hi:
                into_gap = min(angle - lo, hi - angle)
            else:
                into_gap = -min(abs(angle - lo), abs(angle - hi))
            # Convert to pixels at this radius so the cut ends antialias at the
            # same rate as the annulus edges rather than looking ragged.
            gap_px = math.radians(into_gap) * dist
            cover *= max(0.0, min(1.0, 0.5 - gap_px))
            if cover <= 0.0:
                continue
            image.put(x, y, colour, cover)


def make_icon():
    size = ICON_SIZE
    image = Image(size, size, DEEP)

    tile = Image(size, size)
    vertical_gradient(tile, BRIGHT, MID)
    glass_highlight(tile, 0.26)

    radius = size * 0.22
    for y in range(size):
        for x in range(size):
            image.put(x, y, tile.pixels[y][x], rounded_rect_mask(size, size, radius, x, y))

    centre = size / 2.0
    # Drop shadow under the mark, then the mark itself.
    draw_ring_c(image, centre, centre + 2.0, size * 0.31, size * 0.105, DEEP)
    draw_ring_c(image, centre, centre, size * 0.31, size * 0.105, WHITE)

    image.write_png(os.path.join(ASSETS, "icon.png"))


def make_splash(width, height, path, mark_scale):
    image = Image(width, height)
    vertical_gradient(image, MID, DEEP)
    glass_highlight(image, 0.10)

    radius = min(width, height) * mark_scale
    cx = width / 2.0
    cy = height / 2.0
    draw_ring_c(image, cx, cy + radius * 0.05, radius, radius * 0.30, DEEP)
    draw_ring_c(image, cx, cy, radius, radius * 0.30, WHITE)

    image.write_png(path)


def main():
    make_icon()
    make_splash(TV_SIZE[0], TV_SIZE[1], os.path.join(ASSETS, "tv_splash.png"), 0.16)
    make_splash(DRC_SIZE[0], DRC_SIZE[1], os.path.join(ASSETS, "drc_splash.png"), 0.18)


if __name__ == "__main__":
    main()
