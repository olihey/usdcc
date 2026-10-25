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
