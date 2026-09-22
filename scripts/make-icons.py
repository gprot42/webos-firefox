#!/usr/bin/env python3
"""Write icon.png and largeIcon.png.

A flame mark drawn from scratch, in Firefox's colour family. This is not
Mozilla's logo artwork and does not reproduce it.

Needs Pillow. Without it the existing icons are left alone, so an install
on a machine with no Pillow still works.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "app"
SS = 8  # supersample factor, downsampled at the end for clean edges

# Background, deep indigo to violet.
BG_TOP = (26, 18, 66)
BG_BOTTOM = (60, 22, 90)
# Flame, amber through red.
HOT = (255, 214, 92)
MID = (255, 138, 22)
COOL = (226, 47, 42)


def lerp(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def vertical_gradient(size, top, bottom):
    from PIL import Image

    img = Image.new("RGB", (1, size))
    for y in range(size):
        img.putpixel((0, y), lerp(top, bottom, y / max(size - 1, 1)))
    return img.resize((size, size))


def diagonal_gradient(size):
    """Amber at the top-left, red at the bottom-right."""
    from PIL import Image

    img = Image.new("RGB", (size, size))
    px = img.load()
    for y in range(size):
        for x in range(size):
            t = (x / size + y / size) / 2.0
            px[x, y] = lerp(HOT, MID, t * 2) if t < 0.5 else lerp(MID, COOL, (t - 0.5) * 2)
    return img


def build(size):
    """Rounded indigo card with a tapering flame spiral curling into a core."""
    import math

    from PIL import Image, ImageDraw, ImageFilter

    s = size * SS
    cx = cy = s / 2.0

    # Rounded-square background.
    bg_mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(bg_mask).rounded_rectangle(
        [0, 0, s - 1, s - 1], radius=int(s * 0.22), fill=255
    )
    card = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    card.paste(vertical_gradient(s, BG_TOP, BG_BOTTOM), (0, 0), bg_mask)

    # The flame is a spiral stroked with a brush that thins as it curls in,
    # which gives a tapered tail without needing real path geometry.
    flame = Image.new("L", (s, s), 0)
    fd = ImageDraw.Draw(flame)
    turns = 2.45 * math.pi
    radius0 = s * 0.375
    brush0 = s * 0.125
    phase = math.radians(-118)
    steps = 1400
    for i in range(steps + 1):
        f = i / steps
        theta = turns * f
        r = radius0 * (1.0 - 0.80 * f)
        w = brush0 * (1.0 - 0.88 * f) ** 0.85
        if w < 0.5:
            continue
        x = cx + r * math.cos(theta + phase)
        y = cy + r * math.sin(theta + phase)
        fd.ellipse([x - w, y - w, x + w, y + w], fill=255)

    flame = flame.filter(ImageFilter.GaussianBlur(s * 0.005))

    card.paste(diagonal_gradient(s), (0, 0), flame)

    # Bright core at the centre of the curl.
    core = Image.new("L", (s, s), 0)
    cr = s * 0.062
    ImageDraw.Draw(core).ellipse([cx - cr, cy - cr, cx + cr, cy + cr], fill=255)
    core = core.filter(ImageFilter.GaussianBlur(s * 0.006))
    card.paste(Image.new("RGB", (s, s), HOT), (0, 0), core)

    out = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    out.paste(card, (0, 0), bg_mask)
    return out.resize((size, size), Image.LANCZOS)


def main():
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        print("Pillow not installed, leaving the existing icons in place.", file=sys.stderr)
        return
    build(80).save(ROOT / "icon.png")
    build(130).save(ROOT / "largeIcon.png")
    print(f"wrote {ROOT/'icon.png'} and {ROOT/'largeIcon.png'}")


if __name__ == "__main__":
    main()
