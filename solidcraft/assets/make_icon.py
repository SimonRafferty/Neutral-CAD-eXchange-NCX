"""Writes assets/icon.ico: NCX's own mark (an amber rounded square with a white N), the same drawing as the
window icon in src/main.rs, at 16-256 px (PNG entries, Windows Vista and later). Pure Python, no packages.

usage: python assets/make_icon.py
"""
import os
import struct
import zlib


def draw(size):
    s = size / 64.0
    px = bytearray()
    for y in range(size):
        px.append(0)  # PNG filter byte
        for x in range(size):
            fx, fy = (x + 0.5) / s, (y + 0.5) / s
            r = 12.0
            cx = min(max(fx, r), 64 - r)
            cy = min(max(fy, r), 64 - r)
            inside = ((fx - cx) ** 2 + (fy - cy) ** 2) ** 0.5 <= r
            n = (16 <= fx < 24 and 14 <= fy < 50) or (40 <= fx < 48 and 14 <= fy < 50) or \
                (14 <= fy < 50 and abs((fx - 16) - (fy - 14) * (24 / 36)) < 5)
            if inside:
                px += bytes([255, 255, 255, 255]) if n else bytes([0xE8, 0xA3, 0x3D, 255])
            else:
                px += bytes([0, 0, 0, 0])
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(px), 9)) + chunk(b"IEND", b"")


def main():
    sizes = [16, 24, 32, 48, 64, 128, 256]
    pngs = [draw(n) for n in sizes]
    head = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for n, data in zip(sizes, pngs):
        entries += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon.ico")
    with open(out, "wb") as f:
        f.write(head + entries + b"".join(pngs))
    print(out)


if __name__ == "__main__":
    main()
