#!/usr/bin/env python3
"""Writes the PS Vita LiveArea icon (platform/vita/sce_sys/icon0.png): 128x128, 8-bit
indexed, as LiveArea requires. A plain design of the port's own, drawn here so no
image from the game is involved.

  tools/vita_make_icon.py <out.png>
"""
import math
import struct
import sys
import zlib

SIZE = 128
# Palette: background, ring, core, highlight.
PALETTE = [(18, 20, 28), (214, 92, 24), (250, 196, 52), (255, 240, 200)]


def pixel(x, y):
    cx = cy = (SIZE - 1) / 2
    r = math.hypot(x - cx, y - cy)
    if r < 22:
        return 3 if math.hypot(x - cx + 6, y - cy + 6) < 8 else 2
    if 38 <= r < 54:
        return 1
    return 0


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


rows = b"".join(b"\x00" + bytes(pixel(x, y) for x in range(SIZE)) for y in range(SIZE))
png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 3, 0, 0, 0))
       + chunk(b"PLTE", b"".join(bytes(c) for c in PALETTE))
       + chunk(b"IDAT", zlib.compress(rows, 9))
       + chunk(b"IEND", b""))
with open(sys.argv[1], "wb") as f:
    f.write(png)
