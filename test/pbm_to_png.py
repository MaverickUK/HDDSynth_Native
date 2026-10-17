#!/usr/bin/env python3
"""Convert the OLED previews written by the host tests (PBM) to PNG.

    HDDSYNTH_PREVIEW_DIR=docs/oled test/run_native_tests.sh
    python3 test/pbm_to_png.py docs/oled

Each 128x64 screen is scaled 4x, white on black like the panel, with a grey
border. Standard library only.
"""
import os
import struct
import sys
import zlib

SCALE, BORDER = 4, 2


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)))  # 8-bit grey
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def convert(pbm_path):
    tokens = open(pbm_path).read().split()
    width, height = int(tokens[1]), int(tokens[2])
    pixels = [int(t) for t in tokens[3:]]
    w, h = width * SCALE + 2 * BORDER, height * SCALE + 2 * BORDER
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            if y < BORDER or y >= h - BORDER or x < BORDER or x >= w - BORDER:
                row.append(90)
            else:
                row.append(255 if pixels[((y - BORDER) // SCALE) * width + (x - BORDER) // SCALE] else 0)
        rows.append(row)
    write_png(pbm_path[:-4] + ".png", w, h, rows)
    os.remove(pbm_path)


if __name__ == "__main__":
    folder = sys.argv[1]
    for name in sorted(os.listdir(folder)):
        if name.endswith(".pbm"):
            convert(os.path.join(folder, name))
