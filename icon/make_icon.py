#!/usr/bin/env python3
"""
Draws the app icon and writes:
  icon/AppIcon.icns       modern macOS (PNG-based ic07..ic10)
  icon/AppIcon-Tiger.icns Mac OS X 10.4 (classic it32/ih32/il32/is32 + masks)
  icon/AppIcon-1024.png   preview

Design: a white display with a stand, a desktop on its screen with a pointer,
and two signal arcs: "a screen that receives another Mac's display".
"""
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

S = 1024
OUT = Path(__file__).resolve().parent


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def vgradient(size, top, bottom):
    w, h = size
    g = Image.new("RGBA", size)
    px = g.load()
    for y in range(h):
        c = lerp(top, bottom, y / max(h - 1, 1))
        for x in range(w):
            px[x, y] = c
    return g


def rounded_mask(size, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size[0] - 1, size[1] - 1], radius=radius, fill=255)
    return m


def draw_icon(n=S, full_bleed=False):
    k = n / 1024
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))

    if full_bleed:
        # opaque square, no shadow: macOS 26+ applies its own rounded mask
        img.paste(vgradient((n, n), (38, 99, 235, 255), (14, 165, 233, 255)), (0, 0))
    else:
        # background tile (classic macOS icon grid: ~824 px tile centred in 1024)
        tile = int(824 * k)
        off = (n - tile) // 2
        bg = vgradient((tile, tile), (38, 99, 235, 255), (14, 165, 233, 255))
        shadow = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        ImageDraw.Draw(shadow).rounded_rectangle(
            [off, off + int(12 * k), off + tile, off + tile + int(12 * k)], radius=int(185 * k), fill=(0, 0, 0, 90))
        img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(int(18 * k))))
        img.paste(bg, (off, off), rounded_mask((tile, tile), int(185 * k)))

    d = ImageDraw.Draw(img)

    # stand
    cx = n // 2
    neck_w, neck_top, neck_bot = int(70 * k), int(640 * k), int(745 * k)
    d.rectangle([cx - neck_w // 2, neck_top, cx + neck_w // 2, neck_bot], fill=(214, 222, 232, 255))
    d.rounded_rectangle([cx - int(170 * k), neck_bot - int(6 * k), cx + int(170 * k), neck_bot + int(34 * k)],
                        radius=int(18 * k), fill=(236, 241, 247, 255))

    # display body
    bx0, by0, bx1, by1 = int(232 * k), int(268 * k), int(792 * k), int(660 * k)
    body_shadow = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    ImageDraw.Draw(body_shadow).rounded_rectangle([bx0, by0 + int(14 * k), bx1, by1 + int(14 * k)],
                                                  radius=int(40 * k), fill=(0, 0, 0, 80))
    img.alpha_composite(body_shadow.filter(ImageFilter.GaussianBlur(int(14 * k))))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([bx0, by0, bx1, by1], radius=int(40 * k), fill=(248, 250, 252, 255))

    # screen with a little desktop on it
    sx0, sy0, sx1, sy1 = bx0 + int(34 * k), by0 + int(34 * k), bx1 - int(34 * k), by1 - int(34 * k)
    scr = vgradient((sx1 - sx0, sy1 - sy0), (49, 46, 129, 255), (14, 116, 144, 255))
    img.paste(scr, (sx0, sy0), rounded_mask(scr.size, int(14 * k)))
    d = ImageDraw.Draw(img)
    # a window on the desktop
    wx0, wy0, wx1, wy1 = sx0 + int(52 * k), sy0 + int(48 * k), sx0 + int(330 * k), sy0 + int(232 * k)
    d.rounded_rectangle([wx0, wy0, wx1, wy1], radius=int(12 * k), fill=(241, 245, 249, 235))
    d.rounded_rectangle([wx0, wy0, wx1, wy0 + int(30 * k)], radius=int(12 * k), fill=(203, 213, 225, 255))
    d.rectangle([wx0, wy0 + int(18 * k), wx1, wy0 + int(30 * k)], fill=(203, 213, 225, 255))
    for i in range(4):
        yy = wy0 + int((56 + i * 34) * k)
        d.rounded_rectangle([wx0 + int(24 * k), yy, wx0 + int((24 + 200 - i * 38) * k), yy + int(14 * k)],
                            radius=int(7 * k), fill=(148, 163, 184, 255))

    # pointer
    px0, py0 = sx0 + int(370 * k), sy0 + int(150 * k)
    arrow = [(0, 0), (0, 150), (38, 114), (64, 172), (90, 160), (64, 104), (114, 104)]
    pts = [(px0 + x * k, py0 + y * k) for x, y in arrow]
    d.polygon(pts, fill=(255, 255, 255, 255), outline=(15, 23, 42, 255), width=max(1, int(9 * k)))

    # signal arcs, top right of the display
    ax, ay = bx1 - int(10 * k), by0 + int(10 * k)
    for r, wdt in ((70, 22), (126, 22)):
        rr = int(r * k)
        d.arc([ax - rr, ay - rr, ax + rr, ay + rr], start=200, end=250, fill=(255, 255, 255, 255), width=int(wdt * k))
    d.ellipse([ax - int(18 * k), ay - int(18 * k), ax + int(18 * k), ay + int(18 * k)], fill=(255, 255, 255, 255))
    return img


def render(size):
    # draw big, then downsample: crisp at every size
    return draw_icon(S).resize((size, size), Image.LANCZOS)


def full_bleed_big():
    # Modern macOS (26+) frames icons that aren't a full square in a grey
    # tile. So the modern icon is an opaque square; the artwork is enlarged a
    # little (crop 56..968) to fill it, keeping clear of the rounded corners.
    return draw_icon(S, full_bleed=True).crop((56, 56, 968, 968)).resize((S, S), Image.LANCZOS)


def render_modern(size):
    return full_bleed_big().resize((size, size), Image.LANCZOS)


def png_bytes(im):
    from io import BytesIO
    b = BytesIO()
    im.save(b, "PNG")
    return b.getvalue()


def packbits_channel(data):
    """ICNS RLE for one channel (it32/ih32/il32/is32)."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        run = 1
        while i + run < n and run < 130 and data[i + run] == data[i]:
            run += 1
        if run >= 3:
            out += bytes([0x80 + run - 3, data[i]])
            i += run
        else:
            start = i
            while i < n and i - start < 128:
                if i + 2 < n and data[i] == data[i + 1] == data[i + 2]:
                    break
                i += 1
            out += bytes([i - start - 1]) + bytes(data[start:i])
    return bytes(out)


def classic(im, rgb_type, mask_type, prefix=b""):
    im = im.convert("RGBA")
    r, g, b, a = im.split()
    # classic icons store unpremultiplied RGB + separate 8-bit mask
    payload = prefix + b"".join(packbits_channel(ch.tobytes()) for ch in (r, g, b))
    return [(rgb_type, payload), (mask_type, a.tobytes())]


def write_icns(path, chunks):
    body = b"".join(t + struct.pack(">I", len(d) + 8) + d for t, d in chunks)
    path.write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)


def main():
    big = draw_icon(S)
    big.save(OUT / "AppIcon-1024.png")

    fb = full_bleed_big()
    fb.save(OUT / "AppIcon-modern-1024.png")
    rm = lambda n: fb.resize((n, n), Image.LANCZOS)
    modern = [
        (b"ic07", png_bytes(rm(128))),
        (b"ic08", png_bytes(rm(256))),
        (b"ic09", png_bytes(rm(512))),
        (b"ic10", png_bytes(fb)),
        (b"ic11", png_bytes(rm(32))),
        (b"ic12", png_bytes(rm(64))),
        (b"ic13", png_bytes(rm(256))),
        (b"ic14", png_bytes(rm(512))),
    ]
    write_icns(OUT / "AppIcon.icns", modern)

    tiger = []
    tiger += classic(render(128), b"it32", b"t8mk", prefix=b"\0\0\0\0")
    tiger += classic(render(48), b"ih32", b"h8mk")
    tiger += classic(render(32), b"il32", b"l8mk")
    tiger += classic(render(16), b"is32", b"s8mk")
    write_icns(OUT / "AppIcon-Tiger.icns", tiger)
    print("wrote", *(p.name for p in sorted(OUT.glob("AppIcon*"))))


if __name__ == "__main__":
    main()
