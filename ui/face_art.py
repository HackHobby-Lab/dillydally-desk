#!/usr/bin/env python3
"""Render the faces and icons the scene uses.

The faces are soft glowing orbs like the reference picture. GSP has no blur or
radial gradient, so each face is baked as an image on the page background, one
with eyes open and one mid blink. Blinking is then two visibility flips.

Names are internal only. Nothing here is ever shown on screen.

Run from this directory:  python face_art.py
"""

import os
from PIL import Image, ImageDraw, ImageFilter

S = 3  # supersample

INK = (0, 0, 0)  # true black, matches the page
CARD = (0x20, 0x1F, 0x1D)
PAPER = (0xF0, 0xEE, 0xE6)
DIM = (0x8A, 0x87, 0x7F)
BLACK = (17, 17, 17)

# key: (orb colour, expression). Order is the C enum order, see dd_mood.h.
FACES = [
    ("blip",    "#D97757", "idle"),
    ("sunny",   "#F5C65A", "joy"),
    ("meh",     "#6F7ABA", "ennui"),
    ("drizzle", "#79CBFD", "sad"),
    ("jitter",  "#E99557", "anxious"),
    ("wobble",  "#CEB4D7", "fear"),
    ("grump",   "#B94F51", "anger"),
    ("ick",     "#47BB7C", "disgust"),
    ("sparkle", "#419FC3", "envy"),
    ("blush",   "#DB67A6", "embarrassed"),
]

# radius of the ball, canvas edge. The glow needs about 0.35 r of room.
SIZES = {"l": (130, 360), "m": (60, 168), "s": (36, 104)}

# Where the eyes sit for each expression, used to draw closed lids on blink.
# (x, y, half width) in units of r/100.
EYES = {
    "idle":        [(-28, -13, 11), (28, -13, 11)],
    "joy":         [(-30, -14, 18), (30, -14, 18)],
    "ennui":       [(-30, -12, 18), (30, -12, 18)],
    "sad":         [(-30, -16, 18), (30, -16, 18)],
    "anxious":     [(-30, -21, 16), (30, -21, 16)],
    "fear":        [(-24, -26, 13), (24, -26, 13)],
    "anger":       [(-28, -18, 18), (28, -18, 18)],
    "disgust":     [(-28, -12, 18), (28, -14, 18)],
    "envy":        [(-26, -18, 18), (26, -18, 18)],
    "embarrassed": [(-30, -24, 20), (30, -24, 20)],
}


def hexrgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def orb(img, cx, cy, R, base):
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    g = ImageDraw.Draw(glow)
    g.ellipse([cx - R * 1.22, cy - R * 1.18, cx + R * 1.22, cy + R * 1.26],
              fill=base + (105,))
    glow = glow.filter(ImageFilter.GaussianBlur(R * 0.3))
    img.paste(glow, (0, 0), glow)

    ball = Image.new("RGBA", img.size, (0, 0, 0, 0))
    b = ImageDraw.Draw(ball)
    hx, hy = cx - R * 0.3, cy - R * 0.35
    steps = 72
    for i in range(steps):
        t = i / steps
        rr = R * (1 - t)
        ox, oy = (hx - cx) * t, (hy - cy) * t
        c = mix(mix(base, (0, 0, 0), 0.12), mix(base, (255, 255, 255), 0.45), t)
        b.ellipse([cx + ox - rr, cy + oy - rr, cx + ox + rr, cy + oy + rr], fill=c + (255,))
    ball = ball.filter(ImageFilter.GaussianBlur(R * 0.06))
    img.paste(ball, (0, 0), ball)


def features(img, cx, cy, R, kind, base, blink):
    d = ImageDraw.Draw(img)
    k = R / 100.0
    X = lambda v: cx + v * k
    Y = lambda v: cy + v * k
    lw = max(2, int(7 * k))

    def dome(x, y, w, h, up=True):
        a0, a1 = (180, 360) if up else (0, 180)
        d.chord([X(x - w), Y(y - h), X(x + w), Y(y + h)], a0, a1, fill=BLACK)

    def arc(x, y, w, h, a0, a1):
        d.arc([X(x - w), Y(y - h), X(x + w), Y(y + h)], a0, a1, fill=BLACK, width=lw)

    def line(pts):
        d.line([(X(a), Y(b)) for a, b in pts], fill=BLACK, width=lw, joint="curve")

    if blink:
        for (ex, ey, hw) in EYES[kind]:
            arc(ex, ey - 4, hw, 7, 20, 160)
    elif kind == "idle":
        for sx in (-28, 28):
            d.rounded_rectangle([X(sx - 9), Y(-30), X(sx + 9), Y(4)], radius=9 * k, fill=BLACK)
    elif kind == "joy":
        dome(-30, -10, 18, 20)
        dome(30, -10, 18, 20)
    elif kind == "ennui":
        dome(-30, -16, 18, 16, up=False)
        dome(30, -16, 18, 16, up=False)
    elif kind == "sad":
        d.polygon([(X(-46), Y(-12)), (X(-14), Y(-28)), (X(-14), Y(-10)), (X(-40), Y(-2))], fill=BLACK)
        d.polygon([(X(46), Y(-12)), (X(14), Y(-28)), (X(14), Y(-10)), (X(40), Y(-2))], fill=BLACK)
    elif kind == "anxious":
        for sx in (-30, 30):
            d.ellipse([X(sx - 15), Y(-36), X(sx + 15), Y(-6)], fill=BLACK)
    elif kind == "fear":
        dome(-24, -22, 13, 12)
        dome(24, -22, 13, 12)
    elif kind == "anger":
        d.polygon([(X(-46), Y(-34)), (X(-10), Y(-18)), (X(-14), Y(-4)), (X(-44), Y(-14))], fill=BLACK)
        d.polygon([(X(46), Y(-34)), (X(10), Y(-18)), (X(14), Y(-4)), (X(44), Y(-14))], fill=BLACK)
    elif kind == "disgust":
        dome(-28, -14, 18, 14, up=False)
        dome(28, -18, 18, 16, up=False)
    elif kind == "envy":
        for sx in (-26, 26):
            d.ellipse([X(sx - 18), Y(-36), X(sx + 18), Y(0)], fill=BLACK)
            d.ellipse([X(sx - 2), Y(-30), X(sx + 10), Y(-18)], fill=(255, 255, 255))
    elif kind == "embarrassed":
        dome(-30, -18, 20, 22)
        dome(30, -18, 20, 22)

    # mouths stay put through a blink
    if kind == "joy":
        arc(0, 6, 34, 26, 20, 160)
    elif kind == "ennui":
        line([(-18, 26), (18, 22)])
    elif kind == "sad":
        arc(0, 34, 24, 16, 200, 340)
    elif kind == "anxious":
        line([(-46, 14), (46, 14)])
    elif kind == "fear":
        line([(-22, 6), (-12, 0), (-2, 6), (8, 0), (18, 6)])
    elif kind == "anger":
        arc(0, 30, 26, 14, 200, 340)
    elif kind == "disgust":
        line([(-20, 18), (0, 12), (22, 20)])
    elif kind == "envy":
        arc(-16, 14, 12, 10, 20, 160)
    elif kind == "embarrassed":
        for sx in (-44, 44):
            d.ellipse([X(sx - 12), Y(4), X(sx + 12), Y(14)], fill=mix(base, (255, 255, 255), 0.35))


def face_image(color, kind, r, edge, blink):
    img = Image.new("RGB", (edge * S, edge * S), INK)
    c = edge * S / 2
    orb(img, c, c, r * S, hexrgb(color))
    features(img, c, c, r * S, kind, hexrgb(color), blink)
    img = img.resize((edge, edge), Image.LANCZOS)
    # The blurred glow runs past the canvas, and a glow cut off by the image
    # edge shows as a faint square on screen. Fade it to the page colour
    # before the edge so the image has no visible border.
    mask = Image.new("L", (edge, edge), 0)
    md = ImageDraw.Draw(mask)
    steps = 24
    for i in range(steps):
        t = i / (steps - 1)
        rr = edge / 2 * (1.0 - 0.16 * t)
        md.ellipse([edge / 2 - rr, edge / 2 - rr, edge / 2 + rr, edge / 2 + rr],
                   fill=int(255 * t))
    mask = mask.filter(ImageFilter.GaussianBlur(edge * 0.01))
    return Image.composite(img, Image.new("RGB", (edge, edge), INK), mask)


# ------------------------------------------------------------------ icons
def icon(edge, draw_fn, bg=None, radius=0):
    img = Image.new("RGBA", (edge * S, edge * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    if bg:
        d.rounded_rectangle([0, 0, edge * S - 1, edge * S - 1], radius=radius * S, fill=bg)
    draw_fn(d, edge * S)
    return img.resize((edge, edge), Image.LANCZOS)


def g_wifi(color):
    def f(d, e):
        cx, cy = e / 2, e * 0.78
        for rr in (0.62, 0.40):
            R = e * rr
            d.arc([cx - R, cy - R, cx + R, cy + R], 225, 315, fill=color, width=int(e * 0.1))
        R = e * 0.09
        d.ellipse([cx - R, cy - R * 2, cx + R, cy], fill=color)
    return f


def g_wifi_off(color):
    def f(d, e):
        g_wifi(color)(d, e)
        d.line([(e * 0.15, e * 0.2), (e * 0.85, e * 0.9)], fill=color, width=int(e * 0.09))
    return f


def g_play(color):
    def f(d, e):
        d.polygon([(e * 0.34, e * 0.24), (e * 0.34, e * 0.76), (e * 0.78, e * 0.5)], fill=color)
    return f


def g_pause(color):
    def f(d, e):
        d.rounded_rectangle([e * 0.3, e * 0.24, e * 0.44, e * 0.76], radius=e * 0.03, fill=color)
        d.rounded_rectangle([e * 0.56, e * 0.24, e * 0.70, e * 0.76], radius=e * 0.03, fill=color)
    return f


def g_skip(color, forward):
    def f(d, e):
        if forward:
            d.polygon([(e * 0.25, e * 0.22), (e * 0.25, e * 0.78), (e * 0.66, e * 0.5)], fill=color)
            d.rectangle([e * 0.68, e * 0.22, e * 0.77, e * 0.78], fill=color)
        else:
            d.polygon([(e * 0.75, e * 0.22), (e * 0.75, e * 0.78), (e * 0.34, e * 0.5)], fill=color)
            d.rectangle([e * 0.23, e * 0.22, e * 0.32, e * 0.78], fill=color)
    return f


W = (255, 255, 255)


def chip_glyph(kind):
    def f(d, e):
        lw = int(e * 0.09)
        if kind == "wifi":
            cx, cy = e / 2, e * 0.74
            for rr in (0.44, 0.28):
                R = e * rr
                d.arc([cx - R, cy - R, cx + R, cy + R], 225, 315, fill=W, width=lw)
            d.ellipse([cx - e * 0.06, cy - e * 0.12, cx + e * 0.06, cy], fill=W)
        elif kind == "globe":
            d.ellipse([e * 0.22, e * 0.22, e * 0.78, e * 0.78], outline=W, width=lw)
            d.ellipse([e * 0.38, e * 0.22, e * 0.62, e * 0.78], outline=W, width=lw)
            d.line([(e * 0.22, e * 0.5), (e * 0.78, e * 0.5)], fill=W, width=lw)
        elif kind == "cycle":
            d.arc([e * 0.24, e * 0.24, e * 0.76, e * 0.76], 30, 300, fill=W, width=lw)
            d.polygon([(e * 0.66, e * 0.16), (e * 0.82, e * 0.34), (e * 0.6, e * 0.38)], fill=W)
        elif kind == "clock":
            d.ellipse([e * 0.22, e * 0.22, e * 0.78, e * 0.78], outline=W, width=lw)
            d.line([(e * 0.5, e * 0.32), (e * 0.5, e * 0.52), (e * 0.64, e * 0.6)], fill=W, width=lw)
        elif kind == "sun":
            d.ellipse([e * 0.36, e * 0.36, e * 0.64, e * 0.64], fill=W)
            import math
            for i in range(8):
                a = i * math.pi / 4
                d.line([(e / 2 + e * 0.22 * math.cos(a), e / 2 + e * 0.22 * math.sin(a)),
                        (e / 2 + e * 0.32 * math.cos(a), e / 2 + e * 0.32 * math.sin(a))], fill=W, width=lw)
        elif kind == "speaker":
            d.polygon([(e * 0.22, e * 0.4), (e * 0.34, e * 0.4), (e * 0.5, e * 0.26),
                       (e * 0.5, e * 0.74), (e * 0.34, e * 0.6), (e * 0.22, e * 0.6)], fill=W)
            d.arc([e * 0.4, e * 0.3, e * 0.78, e * 0.7], 300, 60, fill=W, width=lw)
        elif kind == "bt":
            # the Bluetooth rune: a spine and two closed loops
            x0, x1 = e * 0.5, e * 0.68
            pts = [(e * 0.32, e * 0.34), (x1, e * 0.64), (x0, e * 0.8), (x0, e * 0.2),
                   (x1, e * 0.36), (e * 0.32, e * 0.66)]
            d.line(pts, fill=W, width=lw, joint="curve")
        elif kind == "bell":
            d.chord([e * 0.28, e * 0.24, e * 0.72, e * 0.8], 180, 360, fill=W)
            d.rectangle([e * 0.28, e * 0.5, e * 0.72, e * 0.66], fill=W)
            d.rectangle([e * 0.22, e * 0.64, e * 0.78, e * 0.7], fill=W)
            d.ellipse([e * 0.44, e * 0.7, e * 0.56, e * 0.8], fill=W)
    return f


CHIPS = [
    ("wifi", "#FF4C01"),
    ("bt", "#3D6FD9"),
    ("globe", "#6F7ABA"),
    ("cycle", "#D97757"),
    ("clock", "#E0A93A"),
    ("sun", "#4FA8D8"),
    ("speaker", "#3FA86C"),
    ("bell", "#DB67A6"),
]


def for_hw_jpeg(img):
    """Pre-compensate for the ESP32-S31 hardware JPEG decoder.

    Measured on the board: the decoder expands colour as if the JPEG were
    limited range, out = (in - 16) * 255 / 219, so the page grey under a face
    came out black and mid tones came out hot. Squeezing the source into
    16..235 first makes the round trip land back on the intended colour. The
    simulator decodes in software and gets the uncompensated set.
    """
    lut = [round(16 + v * 219 / 255) for v in range(256)]
    return img.point(lut * 3)


def main():
    os.makedirs("faces", exist_ok=True)
    os.makedirs("faces_hw", exist_ok=True)
    os.makedirs("icons", exist_ok=True)
    for key, color, kind in FACES:
        for size, (r, edge) in SIZES.items():
            for blink in (False, True):
                img = face_image(color, kind, r, edge, blink)
                name = f"{key}_{size}{'_b' if blink else ''}.png"
                img.save(f"faces/{name}")
                for_hw_jpeg(img).save(f"faces_hw/{name}")

    icon(24, g_wifi(DIM)).save("icons/wifi.png")
    icon(24, g_wifi_off(DIM)).save("icons/wifi_off.png")
    icon(40, g_play(PAPER)).save("icons/play.png")
    icon(40, g_pause(PAPER)).save("icons/pause.png")
    icon(36, g_skip(PAPER, True)).save("icons/next.png")
    icon(36, g_skip(PAPER, False)).save("icons/prev.png")
    for kind, color in CHIPS:
        icon(28, chip_glyph(kind), bg=hexrgb(color), radius=8).save(f"icons/chip_{kind}.png")

    sheet = Image.new("RGB", (10 * 120, 2 * 120), INK)
    for i, (key, _, _) in enumerate(FACES):
        sheet.paste(Image.open(f"faces/{key}_s.png"), (i * 120 + 8, 8))
        sheet.paste(Image.open(f"faces/{key}_s_b.png"), (i * 120 + 8, 128))
    sheet.save("../docs/faces_sheet.png")
    print("faces and icons written")


if __name__ == "__main__":
    main()
