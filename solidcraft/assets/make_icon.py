"""Makes NCX SolidCraft's icon from the drawing in assets/icon-source.jpg (a stepped block between two
chevrons, drawn for the project): the grey background is made transparent and the picture scaled to each size
(a square source keeps its own framing; another shape is cropped and centred in a square).

Writes assets/icon.ico (16-256 px, PNG entries), assets/icon64.rgba (the window icon, raw RGBA 64x64) and
docs/icon.png (the README's picture, 256 px).

Needs Pillow (in the project's .venv).   usage: .venv\\Scripts\\python.exe solidcraft\\assets\\make_icon.py
"""
import io
import os
import struct

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def cut_out(img):
    """RGBA of the drawing with its light grey background transparent (soft at the edges)."""
    img = img.convert("RGB")
    w, h = img.size
    src = img.load()
    out = Image.new("RGBA", (w, h))
    dst = out.load()
    for y in range(h):
        for x in range(w):
            r, g, b = src[x, y]
            sat = max(r, g, b) - min(r, g, b)
            val = (r + g + b) / 3
            # background: unsaturated and light; the drawing: blue-violet faces and near-black outlines
            a = max((sat - 10) / 30.0, (205 - val) / 60.0)
            a = 0.0 if a < 0 else 1.0 if a > 1 else a
            dst[x, y] = (r, g, b, int(round(a * 255)))
    return out


def square(rgba, margin=0.04):
    """A square source is used as framed. Otherwise crop to the drawing and centre it in a transparent square."""
    if rgba.size[0] == rgba.size[1]:
        return rgba
    alpha = rgba.getchannel("A").point(lambda v: 255 if v > 128 else 0)
    box = alpha.getbbox()
    art = rgba.crop(box)
    w, h = art.size
    side = int(max(w, h) * (1 + 2 * margin))
    sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    sq.paste(art, ((side - w) // 2, (side - h) // 2))
    return sq


def scaled(sq, n):
    # premultiplied while resampling, so the edges carry no grey fringe
    return sq.convert("RGBa").resize((n, n), Image.LANCZOS).convert("RGBA")


def png_bytes(im):
    b = io.BytesIO()
    im.save(b, format="PNG", optimize=True)
    return b.getvalue()


def main():
    sq = square(cut_out(Image.open(os.path.join(HERE, "icon-source.jpg"))))
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = {n: scaled(sq, n) for n in sizes}
    pngs = [png_bytes(images[n]) for n in sizes]
    head = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for n, data in zip(sizes, pngs):
        entries += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(os.path.join(HERE, "icon.ico"), "wb") as f:
        f.write(head + entries + b"".join(pngs))
    with open(os.path.join(HERE, "icon64.rgba"), "wb") as f:
        f.write(images[64].tobytes())
    with open(os.path.join(HERE, "..", "docs", "icon.png"), "wb") as f:
        f.write(pngs[-1])
    # the macOS app icon (packaging/macos/make-dmg.sh makes its .icns from this) and the Linux AppImage icon
    with open(os.path.join(HERE, "icon-512.png"), "wb") as f:
        f.write(png_bytes(scaled(sq, 512)))
    print("icon.ico, icon64.rgba, icon-512.png, docs/icon.png written")


if __name__ == "__main__":
    main()
