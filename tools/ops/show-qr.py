#!/usr/bin/env python3
"""Render a 1-bit grayscale QR PNG (as written by the llbot backend) to the
terminal using ANSI half-blocks, so it can be scanned directly from a shell.

Pure stdlib (zlib only) — no Pillow/numpy needed. Handles PNG color type 0,
bit depth 1, non-interlaced, with all five scanline filters.

Usage: show-qr.py [path-to-qrcode.png]   (default: ~/llbot/qrcode.png)
"""
import os
import struct
import sys
import zlib


def decode_png_1bit_gray(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, None, None, bytearray()
    bit_depth = color_type = interlace = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", chunk[:10])
            interlace = chunk[12]
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
        pos += 12 + length
    if (bit_depth, color_type, interlace) != (1, 0, 0):
        raise ValueError("expected 1-bit grayscale non-interlaced PNG, got "
                         f"depth={bit_depth} color={color_type} interlace={interlace}")

    raw = zlib.decompress(bytes(idat))
    stride = (width + 7) // 8            # bytes per scanline
    bpp = 1                              # filter unit for 1-bit gray
    out, prev = [], bytearray(stride)
    p = 0
    for _ in range(height):
        ftype = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            x = line[i]
            if ftype == 1:      # Sub
                x += a
            elif ftype == 2:    # Up
                x += b
            elif ftype == 3:    # Average
                x += (a + b) // 2
            elif ftype == 4:    # Paeth
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                x += a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            line[i] = x & 0xFF
        out.append(line); prev = line
    # expand bits -> dark[y][x]; grayscale value 0 == black(dark)
    dark = [[((out[y][x >> 3] >> (7 - (x & 7))) & 1) == 0 for x in range(width)]
            for y in range(height)]
    return width, height, dark


def min_run(dark, width, height):
    """Smallest run of equal pixels = one QR module in pixels."""
    best = width
    for y in range(0, height, max(1, height // 40) or 1):
        run, cur = 1, dark[y][0]
        for x in range(1, width):
            if dark[y][x] == cur:
                run += 1
            else:
                if 0 < run < best and run > 0:
                    best = run
                run, cur = 1, dark[y][x]
    return max(best, 1)


def render(path):
    width, height, dark = decode_png_1bit_gray(path)
    s = min_run(dark, width, height)
    nx, ny = round(width / s), round(height / s)
    # sample module centers
    grid = [[dark[min(height - 1, y * s + s // 2)][min(width - 1, x * s + s // 2)]
             for x in range(nx)] for y in range(ny)]
    quiet = 2                                   # extra white margin (modules)
    W = nx + 2 * quiet
    padded = ([[False] * W for _ in range(quiet)]
              + [[False] * quiet + row + [False] * quiet for row in grid]
              + [[False] * W for _ in range(quiet)])
    if len(padded) % 2:                         # pad to even rows for half-blocks
        padded.append([False] * W)
    # dark module -> color 0 (black), light -> 7 (white); ▀ upper=fg, lower=bg
    lines = []
    for y in range(0, len(padded), 2):
        cells = []
        for x in range(W):
            top = 0 if padded[y][x] else 7
            bot = 0 if padded[y + 1][x] else 7
            cells.append(f"\033[3{top};4{bot}m▀")
        lines.append("".join(cells) + "\033[0m")
    return "\n".join(lines)


if __name__ == "__main__":
    p = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/llbot/qrcode.png")
    if not os.path.exists(p):
        sys.exit(f"QR file not found: {p}")
    try:
        print(render(p))
    except Exception as e:  # noqa: BLE001
        sys.exit(f"failed to render QR ({p}): {e}")
