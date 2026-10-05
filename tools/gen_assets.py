#!/usr/bin/env python3
"""Generate Cobalt's WUHB artwork (icon + TV/DRC splash screens).

The assets are produced procedurally rather than checked in as opaque binaries
so the palette stays in one place and can be regenerated after a design change.
Pure stdlib on purpose: the Wii U toolchain box is not guaranteed to have
Pillow or ImageMagick available.

    python3 tools/gen_assets.py

It also writes docs/logo.svg, the README mark shared in style with the rest of
the stack (a pixel silhouette on a 3x5 grid, 294 units wide, one `.logo` class,
green in the same two shades Wolfram and MetalBear use). The icon and splash
screens draw the very same grid, so the console and the README carry one mark.

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


# --- The mark ---------------------------------------------------------------
#
# A cut stone: cobalt is a mineral, and the project is named for one. Silhouette only, like Wolfram's wolf and MetalBear's bear; the facets
# are gaps in it, not colour. It is drawn on a grid of cells 3 units wide and 5
# tall in a 294-unit-wide viewBox, which is what the other repositories' logos
# use, so the rects are a run-length encoding of the grid and nothing is placed
# by hand.

CELL_W = 3
CELL_H = 5
LOGO_W = 294
LOGO_H = 270
COLS = LOGO_W // CELL_W
ROWS = LOGO_H // CELL_H


def _inside(poly, x, y):
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def _near_segment(a, b, x, y, half):
    """Whether (x, y) is within `half` units of the segment a-b."""
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / float(dx * dx + dy * dy)))
    return math.hypot(x - (ax + t * dx), y - (ay + t * dy)) <= half


# The cut stone: a table across the top, a wide girdle, a point below. The
# facets are one-cell gaps along the cut lines.
CRYSTALS = [
    ([(66, 0), (228, 0), (294, 95), (147, 270), (0, 95)],
     [((0, 97), (294, 97)),
      ((66, 0), (105, 95)), ((147, 0), (105, 95)), ((147, 0), (189, 95)), ((228, 0), (189, 95)),
      ((105, 95), (147, 270)), ((189, 95), (147, 270))]),
]


def crystal_grid():
    """ROWS x COLS booleans: True where the silhouette is solid."""
    grid = [[False] * COLS for _ in range(ROWS)]
    for row in range(ROWS):
        y = row * CELL_H + CELL_H / 2.0
        for col in range(COLS):
            x = col * CELL_W + CELL_W / 2.0
            for outline, facets in CRYSTALS:
                if _inside(outline, x, y):
                    grid[row][col] = True
                    # A facet is a one-cell gap, so the cut survives the grid.
                    if any(_near_segment(a, b, x, y, 1.6) for a, b in facets):
                        grid[row][col] = False
                    break
    return grid


def write_logo_svg(path):
    grid = crystal_grid()
    rects = []
    for row, cells in enumerate(grid):
        col = 0
        while col < COLS:
            if cells[col]:
                start = col
                while col < COLS and cells[col]:
                    col += 1
                rects.append('<rect x="%d" y="%d" width="%d" height="%d"/>' % (
                    start * CELL_W, row * CELL_H, (col - start) * CELL_W, CELL_H))
            else:
                col += 1
    svg = (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" role="img" '
        'aria-label="Cobalt logo"><style>@media (prefers-color-scheme: dark) '
        '{ .logo { fill: #4ade80; } } @media (prefers-color-scheme: light), '
        '(prefers-color-scheme: no-preference) { .logo { fill: #15803d; } }</style>'
        '<g class="logo" shape-rendering="crispEdges">%s</g></svg>'
    ) % (LOGO_W, LOGO_H, "".join(rects))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as handle:
        handle.write(svg)
    print("wrote %s (%d rects)" % (path, len(rects)))


def draw_crystal(image, cx, cy, height, colour, offset=(0, 0)):
    """Draw the grid centred on (cx, cy), `height` pixels tall, as hard-edged cells."""
    grid = crystal_grid()
    scale = height / float(LOGO_H)
    x0 = cx - LOGO_W * scale / 2.0 + offset[0]
    y0 = cy - LOGO_H * scale / 2.0 + offset[1]
    for row, cells in enumerate(grid):
        ya, yb = int(round(y0 + row * CELL_H * scale)), int(round(y0 + (row + 1) * CELL_H * scale))
        for col, solid in enumerate(cells):
            if not solid:
                continue
            xa, xb = int(round(x0 + col * CELL_W * scale)), int(round(x0 + (col + 1) * CELL_W * scale))
            for y in range(ya, yb):
                for x in range(xa, xb):
                    image.put(x, y, colour)


def make_icon():
    size = ICON_SIZE
    image = Image(size, size, DEEP)

    tile = Image(size, size)
    vertical_gradient(tile, BRIGHT, MID)
    glass_highlight(tile, 0.26)

    # Full-bleed square: the Wii U menu rounds icons itself, so pre-rounded
    # corners show up as dark wedges.
    for y in range(size):
        for x in range(size):
            image.put(x, y, tile.pixels[y][x])

    centre = size / 2.0
    # Drop shadow under the mark, then the mark itself.
    draw_crystal(image, centre, centre, size * 0.70, DEEP, offset=(0, 2))
    draw_crystal(image, centre, centre, size * 0.70, WHITE)

    image.write_png(os.path.join(ASSETS, "icon.png"))


def make_splash(width, height, path, mark_scale):
    image = Image(width, height)
    vertical_gradient(image, MID, DEEP)
    glass_highlight(image, 0.10)

    mark = min(width, height) * mark_scale * 2.4
    cx = width / 2.0
    cy = height / 2.0
    draw_crystal(image, cx, cy, mark, DEEP, offset=(0, mark * 0.02))
    draw_crystal(image, cx, cy, mark, WHITE)

    image.write_png(path)


def main():
    write_logo_svg(os.path.join(os.path.dirname(ASSETS), "docs", "logo.svg"))
    make_icon()
    make_splash(TV_SIZE[0], TV_SIZE[1], os.path.join(ASSETS, "tv_splash.png"), 0.16)
    make_splash(DRC_SIZE[0], DRC_SIZE[1], os.path.join(ASSETS, "drc_splash.png"), 0.18)


if __name__ == "__main__":
    main()
