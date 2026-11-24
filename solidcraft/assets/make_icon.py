"""Draws NCX SolidCraft's icon: the NCX tile (amber, white N), a double-ended arrow, and a stepped block drawn as
a blue line outline (a CAD part). Our own artwork: it is not SolidCraft's logo (theirs is a filled, blue-shaded
isometric S of blocks, under their Brand and Interface Licence); this is a different part, seen from the other
side, as an outline.

Writes assets/icon.ico (16-256 px, PNG entries), assets/icon64.rgba (the window icon, raw RGBA 64x64) and
docs/icon.png (the README's picture).
Pure Python, no packages.   usage: python assets/make_icon.py
"""
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
AMBER = (0xE8, 0xA3, 0x3D)
WHITE = (0xFF, 0xFF, 0xFF)
BLUE = (0x2F, 0x7F, 0xE6)
TINT = (0xEE, 0xF4, 0xFC)


# ---- the drawing, in a 256 x 256 box ---------------------------------------------------------------------
def block_faces(box):
    """A stepped block: an L profile ((0,0) (2,0) (2,1) (1,1) (1,2) (0,2), a step on the left end) extruded 1 deep,
    seen orthographically from the right, front and above. Returns its visible faces as screen polygons in
    painter's order (back to front), fitted into box = (x0, y0, x1, y1)."""
    prof = [(0, 0), (2, 0), (2, 1), (1, 1), (1, 2), (0, 2)]
    faces = [([(x, y, 1) for x, y in prof], (0, 0, 1)), ([(x, y, 0) for x, y in reversed(prof)], (0, 0, -1))]
    for i in range(len(prof)):
        (ax, ay), (bx, by) = prof[i], prof[(i + 1) % len(prof)]
        n = (by - ay, -(bx - ax), 0)  # outward for a counter-clockwise profile
        faces.append(([(ax, ay, 0), (bx, by, 0), (bx, by, 1), (ax, ay, 1)], n))
    v = (1.0, 0.9, 1.3)  # towards the eye
    L = math.sqrt(sum(c * c for c in v))
    v = tuple(c / L for c in v)
    up = (0.0, 1.0, 0.0)
    ex = (up[1] * v[2] - up[2] * v[1], up[2] * v[0] - up[0] * v[2], up[0] * v[1] - up[1] * v[0])
    Le = math.sqrt(sum(c * c for c in ex))
    ex = tuple(-c / Le for c in ex)
    ey = (v[1] * ex[2] - v[2] * ex[1], v[2] * ex[0] - v[0] * ex[2], v[0] * ex[1] - v[1] * ex[0])
    dot = lambda a, b: sum(p * q for p, q in zip(a, b))
    visible = [(f, n) for f, n in faces if dot(n, v) > 1e-9]
    visible.sort(key=lambda fn: sum(dot(p, v) for p in fn[0]) / len(fn[0]))  # far first
    scr = [[(dot(p, ex), -dot(p, ey)) for p in f] for f, _ in visible]
    xs = [x for f in scr for x, _ in f]
    ys = [y for f in scr for _, y in f]
    x0, y0, x1, y1 = box
    s = min((x1 - x0) / (max(xs) - min(xs)), (y1 - y0) / (max(ys) - min(ys)))
    ox = (x0 + x1) / 2 - (min(xs) + max(xs)) / 2 * s
    oy = (y0 + y1) / 2 - (min(ys) + max(ys)) / 2 * s
    return [[(x * s + ox, y * s + oy) for x, y in f] for f in scr]


TILE = (0x20, 0x24, 0x2B)


def shapes():
    """[(kind, geometry, colour)] in drawing order."""
    out = [("rrect", (0, 0, 256, 256, 48), TILE)]
    # the NCX tile with its N
    out.append(("rrect", (8, 74, 116, 182, 18), AMBER))
    out.append(("poly", [(31, 94), (46, 94), (46, 162), (31, 162)], WHITE))
    out.append(("poly", [(78, 94), (93, 94), (93, 162), (78, 162)], WHITE))
    out.append(("poly", [(31, 94), (47, 94), (93, 162), (77, 162)], WHITE))
    # the double-ended arrow between them
    out.append(("line", ((132, 128), (152, 128), 10), AMBER))
    out.append(("poly", [(120, 128), (135, 113), (135, 143)], AMBER))
    out.append(("poly", [(164, 128), (149, 113), (149, 143)], AMBER))
    # the part: light faces with blue outlines (painter's order hides the back edges)
    for poly in block_faces((168, 30, 250, 226)):
        out.append(("poly", poly, TINT))
        for i in range(len(poly)):
            out.append(("line", (poly[i], poly[(i + 1) % len(poly)], 7), BLUE))
    return out


# ---- a small anti-aliased rasteriser ---------------------------------------------------------------------
def inside_poly(x, y, poly):
    c = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            c = not c
        j = i
    return c


def seg_dist(x, y, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0, min(1, ((x - ax) * dx + (y - ay) * dy) / L))
    return math.hypot(x - ax - t * dx, y - ay - t * dy)


def covered(kind, g, x, y):
    if kind == "poly":
        return inside_poly(x, y, g)
    if kind == "line":
        a, b, w = g
        return seg_dist(x, y, a, b) <= w / 2
    if kind == "rrect":
        x0, y0, x1, y1, r = g
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return math.hypot(x - cx, y - cy) <= r
    return False


def render(size, ss=4):
    sh = shapes()
    k = 256.0 / size
    px = bytearray()
    offs = [(i + 0.5) / ss for i in range(ss)]
    for py in range(size):
        for pxl in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for oy in offs:
                for ox in offs:
                    x, y = (pxl + ox) * k, (py + oy) * k
                    col = None
                    for kind, g, c in sh:
                        if covered(kind, g, x, y):
                            col = c
                    if col:
                        acc[0] += col[0]; acc[1] += col[1]; acc[2] += col[2]; acc[3] += 1
            n = ss * ss
            if acc[3]:
                px += bytes([int(acc[0] / acc[3]), int(acc[1] / acc[3]), int(acc[2] / acc[3]), int(255 * acc[3] / n)])
            else:
                px += bytes([0, 0, 0, 0])
    return bytes(px)


def png(size, rgba):
    raw = b"".join(b"\x00" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = {n: render(n) for n in sizes}
    pngs = [png(n, images[n]) for n in sizes]
    head = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for n, data in zip(sizes, pngs):
        entries += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(os.path.join(HERE, "icon.ico"), "wb") as f:
        f.write(head + entries + b"".join(pngs))
    with open(os.path.join(HERE, "icon64.rgba"), "wb") as f:
        f.write(images[64])
    # the 256 px picture the README shows
    with open(os.path.join(HERE, "..", "docs", "icon.png"), "wb") as f:
        f.write(pngs[-1])
    print("icon.ico, icon64.rgba, docs/icon.png written")


if __name__ == "__main__":
    main()
