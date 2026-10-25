"""Renders the usdcc app icon to PNGs (for Qt) and a multi-size Windows .ico.

The artwork is a 1024x1024 SVG: three stacked blue diamond layers on a dark
rounded tile, the front layer framed. It is rasterised here from its
geometry instead of loading the SVG at runtime because the deployed Qt has no
SVG plugin. Each size is rendered natively (4x4 supersampling) rather than
scaled down from one master, so small sizes stay crisp.

Geometry (all in the SVG's 1024 space, inside a group scaled 1.25x about the
centre):
  * tile: 1024x1024 rounded rect, rx=228, ground colour
  * for each of back, mid, front: a 56-wide stroke in the ground colour painted
    *under* the fill (SVG paint-order: stroke), i.e. a 28px ground-coloured halo
    around each layer, then the layer's diamond fill
  * the front layer's frame: a ground diamond with a smaller back-colour
    diamond inside it

Standard library only. Run from the repo root:
    python tools/gen_app_icon.py
Outputs:
    src/cpp/ui/resources/app-icon/usdcc-<size>.png   (loaded by theme::appIcon())
    src/cpp/app/resources/usdcc.ico                  (Windows: the .exe's icon, via usdcc.rc)
    src/cpp/app/resources/usdcc.icns                 (macOS: the app bundle's icon)
    src/cpp/app/resources/linux/icons/hicolor/...    (Linux: freedesktop icon theme tree,
                                                      installed to share/icons; usdcc.desktop
                                                      next to it refers to it as "usdcc")
"""

import math
import struct
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PNG_DIR = REPO / "src/cpp/ui/resources/app-icon"
ICO_PATH = REPO / "src/cpp/app/resources/usdcc.ico"

ICNS_PATH = REPO / "src/cpp/app/resources/usdcc.icns"
LINUX_ICON_DIR = REPO / "src/cpp/app/resources/linux/icons/hicolor"

SIZES = [16, 24, 32, 48, 64, 128, 256]  # Qt resource PNGs + the .ico frames
LINUX_SIZES = [16, 24, 32, 48, 64, 128, 256, 512]  # freedesktop hicolor "apps" sizes
# macOS .icns entries: OSType -> pixel size. The "@2x" types (ic11-ic14) are the
# retina variants of the 16/32/128/256pt icons, so they reuse the 32/64/256/512px renders.
ICNS_TYPES = [
    (b"icp4", 16), (b"icp5", 32), (b"icp6", 64), (b"ic07", 128), (b"ic08", 256),
    (b"ic09", 512), (b"ic10", 1024),
    (b"ic11", 32), (b"ic12", 64), (b"ic13", 256), (b"ic14", 512),
]

# The SVG left these as template placeholders; chosen from the dark theme's
# tokens (see ui/resources/theme/dark.qss): ground = panel surface, front =
# accent, back = the design's deep material blue.
GROUND = (0x1C, 0x1C, 0x1E)
BACK = (0x1D, 0x4F, 0x96)
MID = (0x2D, 0x6F, 0xD1)
FRONT = (0x3D, 0x8B, 0xFD)

TILE_RADIUS = 228
GROUP_SCALE = 1.25
HALO = 28  # half of the SVG's 56 stroke width

# (points, fill) in painting order — each gets its ground halo first.
LAYERS = [
    ([(232, 632), (512, 492), (792, 632), (512, 772)], BACK),
    ([(232, 512), (512, 372), (792, 512), (512, 652)], MID),
    ([(232, 392), (512, 252), (792, 392), (512, 532)], FRONT),
]
FRAME_OUTER = [(333, 392), (512, 302), (691, 392), (512, 482)]
FRAME_INNER = [(389, 392), (512, 330), (635, 392), (512, 454)]


def to_final(pt):
    """Apply the SVG group transform: scale 1.25 about (512, 512)."""
    return (512 + GROUP_SCALE * (pt[0] - 512), 512 + GROUP_SCALE * (pt[1] - 512))


def convex_halfplanes(points, grow=0.0):
    """Half-planes (nx, ny, ax, ay, offset) of a convex polygon, optionally grown
    outward by `grow` with mitred corners (which is what the SVG's default
    stroke-linejoin produces for these shapes — all corners are well inside the
    default miter limit of 4)."""
    pts = [to_final(p) for p in points]
    cx = sum(p[0] for p in pts) / len(pts)
    cy = sum(p[1] for p in pts) / len(pts)
    planes = []
    for i, a in enumerate(pts):
        b = pts[(i + 1) % len(pts)]
        dx, dy = b[0] - a[0], b[1] - a[1]
        length = math.hypot(dx, dy)
        nx, ny = dy / length, -dx / length
        if nx * (cx - a[0]) + ny * (cy - a[1]) > 0:  # make the normal point outward
            nx, ny = -nx, -ny
        planes.append((nx, ny, a[0], a[1], grow))
    return planes


def inside(planes, x, y):
    for nx, ny, ax, ay, off in planes:
        if nx * (x - ax) + ny * (y - ay) > off:
            return False
    return True


def in_tile(x, y):
    """Rounded square tile: 0..1024 with corner radius TILE_RADIUS."""
    if x < 0 or y < 0 or x > 1024 or y > 1024:
        return False
    r = TILE_RADIUS
    cx = min(max(x, r), 1024 - r)
    cy = min(max(y, r), 1024 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


# Painting order, each as (shape-test, colour); later entries overwrite earlier.
def build_paint_list():
    paints = []
    for points, colour in LAYERS:
        paints.append((convex_halfplanes(points, grow=HALO * GROUP_SCALE), GROUND))
        paints.append((convex_halfplanes(points), colour))
    paints.append((convex_halfplanes(FRAME_OUTER), GROUND))
    paints.append((convex_halfplanes(FRAME_INNER), BACK))
    return paints


def render(size, paints, ss=4):
    rows = []
    step = 1024.0 / size
    for py in range(size):
        row = bytearray([0])  # PNG filter 0
        for px in range(size):
            r_sum = g_sum = b_sum = opaque = 0
            for sy in range(ss):
                for sx in range(ss):
                    x = (px + (sx + 0.5) / ss) * step
                    y = (py + (sy + 0.5) / ss) * step
                    if not in_tile(x, y):
                        continue
                    colour = GROUND
                    for planes, c in paints:
                        if inside(planes, x, y):
                            colour = c
                    r_sum += colour[0]
                    g_sum += colour[1]
                    b_sum += colour[2]
                    opaque += 1
            if opaque == 0:
                row += bytes((0, 0, 0, 0))
            else:
                row += bytes(
                    (round(r_sum / opaque), round(g_sum / opaque), round(b_sum / opaque),
                     round(255 * opaque / (ss * ss)))
                )
        rows.append(bytes(row))
    return b"".join(rows)  # with a leading filter byte per row


def png_bytes(size, raw):
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def dib_bytes(size, raw):
    """The 32-bit BGRA DIB (plus empty AND mask) a classic .ico frame stores."""
    stride = 1 + size * 4
    pixels = bytearray()
    for y in range(size - 1, -1, -1):  # bottom-up
        row = raw[y * stride + 1 : (y + 1) * stride]
        for x in range(size):
            r, g, b, a = row[x * 4 : x * 4 + 4]
            pixels += bytes((b, g, r, a))
    mask_stride = ((size + 31) // 32) * 4
    mask = bytes(mask_stride * size)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(pixels) + len(mask), 0, 0, 0, 0)
    return header + bytes(pixels) + mask


def write_ico(path, frames):
    """frames: list of (size, bytes, is_png). Sizes < 256 use DIB (best compatibility
    with the resource compiler); 256 uses PNG, as the format expects."""
    header = struct.pack("<HHH", 0, 1, len(frames))
    offset = 6 + 16 * len(frames)
    entries = b""
    body = b""
    for size, data, _ in frames:
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset + len(body))
        body += data
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + entries + body)


def write_icns(path, pngs_by_size):
    """An .icns is 'icns' + total length, then (OSType, length, PNG data) entries —
    modern macOS reads PNG payloads for all of these types."""
    body = b""
    for ostype, size in ICNS_TYPES:
        data = pngs_by_size[size]
        body += ostype + struct.pack(">I", 8 + len(data)) + data
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"icns" + struct.pack(">I", 8 + len(body)) + body)


def hex_colour(c):
    return "#%02x%02x%02x" % c


def svg_text():
    """The same artwork as the source SVG, with the colour placeholders filled in. The
    stroke halo is spelled out as a stroked polygon drawn before its fill (instead of
    paint-order="stroke") so every SVG renderer — librsvg, KDE, browsers — agrees."""

    def points(pts):
        return " ".join(f"{x},{y}" for x, y in pts)

    ground = hex_colour(GROUND)
    layers = ""
    for pts, colour in LAYERS:
        layers += (
            f'    <polygon points="{points(pts)}" fill="{ground}" stroke="{ground}" '
            f'stroke-width="{HALO * 2}" stroke-linejoin="miter"/>\n'
            f'    <polygon points="{points(pts)}" fill="{hex_colour(colour)}"/>\n'
        )
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" height="1024">\n'
        '  <title>usdcc</title>\n'
        f'  <rect width="1024" height="1024" rx="{TILE_RADIUS}" fill="{ground}"/>\n'
        '  <g transform="translate(512 512) scale(1.25) translate(-512 -512)">\n'
        f"{layers}"
        f'    <polygon points="{points(FRAME_OUTER)}" fill="{ground}"/>\n'
        f'    <polygon points="{points(FRAME_INNER)}" fill="{hex_colour(BACK)}"/>\n'
        "  </g>\n"
        "</svg>\n"
    )


def main():
    paints = build_paint_list()
    PNG_DIR.mkdir(parents=True, exist_ok=True)

    needed = sorted(set(SIZES) | set(LINUX_SIZES) | {size for _, size in ICNS_TYPES})
    pngs = {}  # size -> PNG bytes
    raws = {}
    for size in needed:
        # Fewer samples for the huge sizes: edges are already smooth at that resolution
        # and a pure-Python render of 1024px at 4x4 is slow.
        ss = 4 if size <= 256 else (3 if size == 512 else 2)
        raws[size] = render(size, paints, ss)
        pngs[size] = png_bytes(size, raws[size])
        print("rendered", size)

    frames = []
    for size in SIZES:
        (PNG_DIR / f"usdcc-{size}.png").write_bytes(pngs[size])
        frames.append((size, pngs[size] if size >= 256 else dib_bytes(size, raws[size]), size >= 256))
    write_ico(ICO_PATH, frames)
    print("wrote", ICO_PATH)

    write_icns(ICNS_PATH, pngs)
    print("wrote", ICNS_PATH)

    for size in LINUX_SIZES:
        target = LINUX_ICON_DIR / f"{size}x{size}" / "apps"
        target.mkdir(parents=True, exist_ok=True)
        (target / "usdcc.png").write_bytes(pngs[size])
    scalable = LINUX_ICON_DIR / "scalable" / "apps"
    scalable.mkdir(parents=True, exist_ok=True)
    (scalable / "usdcc.svg").write_text(svg_text(), encoding="utf-8", newline="\n")
    print("wrote", LINUX_ICON_DIR)


if __name__ == "__main__":
    main()
