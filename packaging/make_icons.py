#!/usr/bin/env python3
"""Draws Papertrail's icon at each size Windows and the window need, from the same shapes as
packaging/linux/io.github.hachigodesu.Papertrail.svg, with no libraries beyond Python's own.

Usage: python packaging/make_icons.py

Writes packaging/windows/papertrail.ico and src/app/icons/papertrail-<size>.png. Run it again after
changing the shapes here and in the SVG together.
"""
import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent

# x, y, width, height, corner radius and colour, on a 256 by 256 canvas, back to front. As in the SVG.
SHAPES = [
    (0, 0, 256, 256, 44, (0x1F, 0x4E, 0x5F)),
    (84, 44, 116, 152, 8, (0x9F, 0xC3, 0xCC)),
    (68, 60, 116, 152, 8, (0xF7, 0xF4, 0xEC)),
    (88, 92, 76, 9, 4, (0x1F, 0x4E, 0x5F)),
    (88, 116, 76, 9, 4, (0x7A, 0x97, 0xA0)),
    (88, 140, 76, 9, 4, (0x7A, 0x97, 0xA0)),
    (88, 164, 48, 9, 4, (0x7A, 0x97, 0xA0)),
]
SAMPLES = 4  # Per side of each pixel, for smooth edges.
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
PNG_SIZES = [16, 32, 48, 64, 128, 256]


def inside(px, py, shape):
    x, y, w, h, r, _ = shape
    if px < x or py < y or px > x + w or py > y + h:
        return False
    # Nearest point of the rectangle shrunk by the radius, which the corner must be within r of.
    cx = min(max(px, x + r), x + w - r)
    cy = min(max(py, y + r), y + h - r)
    return (px - cx) ** 2 + (py - cy) ** 2 <= r * r


def render(size):
    """Straight RGBA rows, top to bottom."""
    scale = 256.0 / size
    rows = []
    for row in range(size):
        line = bytearray()
        for column in range(size):
            red = green = blue = alpha = 0.0
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    px = (column + (sx + 0.5) / SAMPLES) * scale
                    py = (row + (sy + 0.5) / SAMPLES) * scale
                    for shape in reversed(SHAPES):  # The frontmost shape covering this point.
                        if inside(px, py, shape):
                            colour = shape[5]
                            red += colour[0]
                            green += colour[1]
                            blue += colour[2]
                            alpha += 1
                            break
            n = SAMPLES * SAMPLES
            if alpha:
                line += bytes((round(red / alpha), round(green / alpha), round(blue / alpha), round(255 * alpha / n)))
            else:
                line += bytes(4)
        rows.append(bytes(line))
    return rows


def png(size, rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + line for line in rows)
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main():
    images = {size: png(size, render(size)) for size in SIZES}

    # An ICO of PNG images, which every Windows from Vista on reads.
    directory = struct.pack("<HHH", 0, 1, len(SIZES))
    offset = 6 + 16 * len(SIZES)
    body = b""
    for size in SIZES:
        data = images[size]
        directory += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        body += data
    ico = ROOT / "packaging" / "windows" / "papertrail.ico"
    ico.parent.mkdir(parents=True, exist_ok=True)
    ico.write_bytes(directory + body)

    icons = ROOT / "src" / "app" / "icons"
    icons.mkdir(parents=True, exist_ok=True)
    for size in PNG_SIZES:
        (icons / f"papertrail-{size}.png").write_bytes(images[size])
    print(f"Wrote {ico.relative_to(ROOT)} and {len(PNG_SIZES)} PNGs in {icons.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
