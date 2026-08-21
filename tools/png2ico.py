#!/usr/bin/env python3
"""Assemble a multi-size Windows .ico from PNG files.

Why not `magick a.png b.png ... out.ico`: ImageMagick stores every entry as an
uncompressed 32-bit DIB, which turns POM2.ico from ~25 KB into 370 KB — all of
it embedded into POM2.exe by POM2.rc, for no benefit.

Every entry here is the PNG file verbatim. That is what the .ico this replaced
already did (checked entry by entry before switching), so it is proven on the
platforms POM2 ships to; Windows has read PNG icon entries at any size since
Vista. Only pre-Vista shells need BITMAPINFOHEADER payloads for sizes under
256, and POM2 does not target them.

Usage: png2ico.py OUT.ico IN16.png IN32.png ...   (any order; sizes are read
from each PNG's IHDR, and entries are written smallest-first)
"""

import struct
import sys


def png_size(path):
    """(width, height) from the PNG header, without decoding pixels."""
    with open(path, "rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        raise SystemExit(f"png2ico: {path} is not a PNG")
    return struct.unpack(">II", head[16:24])


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    out_path, png_paths = argv[1], argv[2:]

    entries = []
    for path in png_paths:
        w, h = png_size(path)
        if w != h:
            raise SystemExit(f"png2ico: {path} is {w}x{h}, want a square")
        if w > 256:
            raise SystemExit(f"png2ico: {path} is {w}px; .ico tops out at 256")
        with open(path, "rb") as f:
            entries.append((w, f.read()))
    entries.sort(key=lambda e: e[0])

    offset = 6 + 16 * len(entries)
    directory = b""
    for w, payload in entries:
        directory += struct.pack("<BBBBHHII",
                                 0 if w == 256 else w,   # 0 encodes 256
                                 0 if w == 256 else w,
                                 0,     # colours in palette: 0 = truecolour
                                 0,     # reserved
                                 1,     # colour planes
                                 32,    # bits per pixel
                                 len(payload), offset)
        offset += len(payload)

    with open(out_path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(entries)))   # reserved, type=ICO
        f.write(directory)
        for _, payload in entries:
            f.write(payload)


if __name__ == "__main__":
    main(sys.argv)
