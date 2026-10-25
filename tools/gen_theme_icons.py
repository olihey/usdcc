"""Generates the small PNG icons the dark Qt theme references from its stylesheets.

PNG rather than SVG on purpose: Qt's SVG image/icon plugins are not part of
the deployed app, so `image: url(foo.svg)` in a stylesheet would silently draw
nothing. Each icon is written at 1x (16px) and 2x (32px, `@2x`) — Qt picks the
@2x file automatically on high-DPI displays.

Standard library only. Run from the repo root:
    python tools/gen_theme_icons.py
"""

import math
import struct
import zlib
from pathlib import Path

OUT_DIR = Path(__file__).resolve().parent.parent / "src/cpp/ui/resources/theme"

LIGHT = (160, 160, 164)
DIM = (74, 74, 78)
WHITE = (255, 255, 255)

# name -> (stroke width in 16px space, rgb, list of polylines in 16px space)
ICONS = {
    "chevron-down": (1.4, LIGHT, [[(4.5, 6.5), (8, 10), (11.5, 6.5)]]),
    "check": (1.8, WHITE, [[(3.5, 8.5), (6.5, 11.5), (12.5, 4.5)]]),
    "close": (1.4, LIGHT, [[(5, 5), (11, 11)], [(11, 5), (5, 11)]]),
    "close-disabled": (1.4, DIM, [[(5, 5), (11, 11)], [(11, 5), (5, 11)]]),
    # Custom title-bar caption buttons (MainWindow's QWindowKit title bar).
    "win-minimize": (1.0, LIGHT, [[(3.5, 8), (12.5, 8)]]),
    "win-maximize": (1.0, LIGHT, [[(3.5, 3.5), (12.5, 3.5), (12.5, 12.5), (3.5, 12.5), (3.5, 3.5)]]),
    "win-restore": (
        1.0,
        LIGHT,
        [
            [(3.5, 5.5), (10.5, 5.5), (10.5, 12.5), (3.5, 12.5), (3.5, 5.5)],
            [(5.5, 5.5), (5.5, 3.5), (12.5, 3.5), (12.5, 10.5), (10.5, 10.5)],
        ],
    ),
    "win-close": (1.1, LIGHT, [[(4, 4), (12, 12)], [(12, 4), (4, 12)]]),
    "win-close-hover": (1.1, WHITE, [[(4, 4), (12, 12)], [(12, 4), (4, 12)]]),
    "detach": (
        1.3,
        LIGHT,
        [
            [(3.5, 6.5), (9.5, 6.5), (9.5, 12.5), (3.5, 12.5), (3.5, 6.5)],
            [(8, 8), (12.5, 3.5)],
            [(9.5, 3.5), (12.5, 3.5), (12.5, 6.5)],
        ],
    ),
    "detach-disabled": (
        1.3,
        DIM,
        [
            [(3.5, 6.5), (9.5, 6.5), (9.5, 12.5), (3.5, 12.5), (3.5, 6.5)],
            [(8, 8), (12.5, 3.5)],
            [(9.5, 3.5), (12.5, 3.5), (12.5, 6.5)],
        ],
    ),
}

def _arc(cx, cy, r, start_deg, end_deg, steps=24):
    return [
        (cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a)))
        for a in (start_deg + (end_deg - start_deg) * i / steps for i in range(steps + 1))
    ]


def _arrowhead(tip, direction_deg, length=2.6, spread=2.2):
    """Two wing strokes of an arrowhead whose tip is at `tip`, pointing along
    `direction_deg` (screen coordinates: +x right, +y down)."""
    d = math.radians(direction_deg)
    back = (-math.cos(d), -math.sin(d))
    side = (-math.sin(d), math.cos(d))
    base = (tip[0] + back[0] * length, tip[1] + back[1] * length)
    return [
        [(base[0] + side[0] * spread, base[1] + side[1] * spread), tip, (base[0] - side[0] * spread, base[1] - side[1] * spread)]
    ]


# Viewport tool glyphs (the tool rail). Each is drawn twice: light for the
# idle state, white for the checked state on the accent background.
_ROTATE_ARC = _arc(8, 8, 5.0, -40, 220)
_ROTATE_TIP = _ROTATE_ARC[-1]
_ROTATE_DIR = 220 + 90  # tangent of a clockwise-on-screen sweep at its end
TOOL_GLYPHS = {
    "tool-select": (
        1.2,
        [[(4.5, 3.0), (4.5, 12.5), (7.0, 10.2), (8.8, 13.6), (10.4, 12.8), (8.6, 9.5), (12.0, 9.3), (4.5, 3.0)]],
    ),
    "tool-move": (
        1.2,
        [
            [(2.5, 8), (13.5, 8)],
            [(8, 2.5), (8, 13.5)],
            [(6, 4.5), (8, 2.5), (10, 4.5)],
            [(6, 11.5), (8, 13.5), (10, 11.5)],
            [(4.5, 6), (2.5, 8), (4.5, 10)],
            [(11.5, 6), (13.5, 8), (11.5, 10)],
        ],
    ),
    "tool-rotate": (1.2, [_ROTATE_ARC] + _arrowhead(_ROTATE_TIP, _ROTATE_DIR)),
    "tool-scale": (
        1.2,
        [
            [(3.5, 8.5), (7.5, 8.5), (7.5, 12.5), (3.5, 12.5), (3.5, 8.5)],
            [(8.5, 7.5), (12.5, 3.5)],
            [(8.5, 3.5), (12.5, 3.5), (12.5, 7.5)],
        ],
    ),
}
# Drag-handle grips for movable toolbars: two columns of dots (for the handle
# on a horizontal toolbar) and two rows (for a vertical one). A zero-length
# stroke with round caps is a dot.
GRIP = (104, 104, 110)
ICONS["grip-horizontal"] = (2.0, GRIP, [[(x, y), (x, y)] for x in (6.5, 9.5) for y in (4.5, 8, 11.5)])
ICONS["grip-vertical"] = (2.0, GRIP, [[(x, y), (x, y)] for y in (6.5, 9.5) for x in (4.5, 8, 11.5)])

for _name, (_width, _lines) in TOOL_GLYPHS.items():
    ICONS[_name] = (_width, LIGHT, _lines)
    ICONS[_name + "-active"] = (_width, WHITE, _lines)

SUPERSAMPLE = 4


def dist_to_segment(px, py, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    length_sq = dx * dx + dy * dy
    t = 0.0 if length_sq == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length_sq))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def render(size, width, rgb, polylines):
    scale = size / 16.0
    segments = []
    for line in polylines:
        for a, b in zip(line, line[1:]):
            segments.append(((a[0] * scale, a[1] * scale), (b[0] * scale, b[1] * scale)))
    radius = width * scale / 2.0

    rows = []
    for y in range(size):
        row = bytearray([0])  # PNG filter type 0 (None)
        for x in range(size):
            hits = 0
            for sy in range(SUPERSAMPLE):
                for sx in range(SUPERSAMPLE):
                    px = x + (sx + 0.5) / SUPERSAMPLE
                    py = y + (sy + 0.5) / SUPERSAMPLE
                    if any(dist_to_segment(px, py, a, b) <= radius for a, b in segments):
                        hits += 1
            alpha = round(255 * hits / (SUPERSAMPLE * SUPERSAMPLE))
            row += bytes((rgb[0], rgb[1], rgb[2], alpha))
        rows.append(bytes(row))
    return b"".join(rows)


def write_png(path, size, raw):
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for name, (width, rgb, polylines) in ICONS.items():
        write_png(OUT_DIR / f"{name}.png", 16, render(16, width, rgb, polylines))
        write_png(OUT_DIR / f"{name}@2x.png", 32, render(32, width, rgb, polylines))
        print("wrote", name)


if __name__ == "__main__":
    main()
