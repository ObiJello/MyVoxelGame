#!/usr/bin/env python3
"""Generate the merchant screen's reroll-trades button icons.

Writes three 16x16 RGBA sprites under
assets/textures/gui/sprites/container/villager/ (picked up by GuiAtlas like
every other gui sprite):

  reroll.png              the refresh glyph (one clockwise arrow), light grey
  reroll_highlighted.png  the same glyph for the hovered button (MC's old
                          hovered-button text colour, 0xFFFFA0)
  reroll_locked.png       the glyph greyed out (MC's disabled text 0xA0A0A0)
                          under a red slash -- the villager has traded and its
                          trades are locked

The icon is drawn over a 20x20 widget/button (or button_disabled) at +2,+2,
so it follows the pack's button frame. Pixel style follows MC's gui text: a
flat fill with a one-pixel shadow down-right at a quarter of the colour.

Usage:  python3 tools/gen_villager_reroll_icons.py [--preview out.png]
"""

import argparse
import math
import os

from PIL import Image

SIZE = 16
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(REPO, "assets", "textures", "gui", "sprites", "container", "villager")


def glyph_pixels():
    """The refresh glyph: one clockwise arrow. A 2-pixel ring (an 11-pixel
    circle) open at the top right, whose arc ends just past 12 o'clock in a
    solid triangular head pointing along the direction of travel."""
    cx = cy = 7.5
    ring = set()
    for y in range(SIZE):
        for x in range(SIZE):
            px, py = x + 0.5, y + 0.5
            if not (3.5 <= math.hypot(px - cx, py - cy) <= 5.5):
                continue
            # Angle clockwise from 12 o'clock; the gap runs from just past
            # 12 to 3 o'clock.
            ang = math.degrees(math.atan2(px - cx, cy - py)) % 360.0
            if 10.0 <= ang <= 100.0:
                continue
            ring.add((x, y))
    # The head: three columns tapering 6 / 4 / 2 pixels, centred on the
    # stroke's two top rows, tip to the right.
    head = {(8, y) for y in range(0, 6)} | {(9, y) for y in range(1, 5)} | {(10, 2), (10, 3)}
    # The shape spans x 2..12, y 0..12; shift it to sit centred in the cell
    # (the one-pixel shadow adds to the right and bottom).
    return {(x + 1, y + 1) for (x, y) in ring | head}


def slash_pixels():
    """A 2-pixel '\\' from the top-left to the bottom-right corner."""
    pixels = set()
    for i in range(1, SIZE - 2):
        pixels.add((i, i))
        pixels.add((i + 1, i))
    return pixels


def shadow_of(rgb):
    # MC Font drop shadow: the colour at a quarter brightness.
    return tuple(c // 4 for c in rgb)


def draw(img, pixels, rgb, shadow_over=False):
    # shadow_over: the shadow also covers what is already drawn (the slash
    # cuts its own dark edge into the glyph under it).
    sh = shadow_of(rgb) + (255,)
    for (x, y) in pixels:
        sx, sy = x + 1, y + 1
        if sx < SIZE and sy < SIZE and (sx, sy) not in pixels:
            if shadow_over or img.getpixel((sx, sy))[3] == 0:
                img.putpixel((sx, sy), sh)
    for (x, y) in pixels:
        img.putpixel((x, y), rgb + (255,))


def make_icon(fill):
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw(img, glyph_pixels(), fill)
    return img


def make_locked():
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    slash = slash_pixels()
    # Clear the glyph just above the slash; the slash's own shadow cuts the
    # edge below it, so the red line reads cleanly over the arrows.
    glyph = {(x, y) for (x, y) in glyph_pixels() if (x, y + 1) not in slash and (x, y) not in slash}
    draw(img, glyph, (0xA0, 0xA0, 0xA0))
    draw(img, slash, (0xFF, 0x55, 0x55), shadow_over=True)   # ChatFormatting.RED
    return img


def preview(icons, path):
    """The three icons on 20x20 buttons, 8x, for eyeballing."""
    sprites = os.path.join(REPO, "assets", "textures", "gui", "sprites", "widget")

    def button(name):
        src = Image.open(os.path.join(sprites, name + ".png")).convert("RGBA")
        # nine-slice at 20 wide: the left and right 10 columns of the 200.
        b = Image.new("RGBA", (20, 20))
        b.paste(src.crop((0, 0, 10, 20)), (0, 0))
        b.paste(src.crop((190, 0, 200, 20)), (10, 0))
        return b

    frames = [("button", icons[0]), ("button_highlighted", icons[1]), ("button_disabled", icons[2])]
    sheet = Image.new("RGBA", (len(frames) * 24, 20), (198, 198, 198, 255))
    for i, (frame, icon) in enumerate(frames):
        b = button(frame)
        b.alpha_composite(icon, (2, 2))
        sheet.paste(b, (i * 24, 0))
    big = sheet.resize((sheet.width * 8, sheet.height * 8), Image.NEAREST)
    small = sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST)
    out = Image.new("RGBA", (big.width, big.height + small.height + 8), (198, 198, 198, 255))
    out.paste(big, (0, 0))
    out.paste(small, (0, big.height + 8))
    out.save(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", help="also write an 8x preview sheet here")
    args = parser.parse_args()

    icons = [
        make_icon((0xE0, 0xE0, 0xE0)),
        make_icon((0xFF, 0xFF, 0xA0)),
        make_locked(),
    ]
    os.makedirs(OUT_DIR, exist_ok=True)
    for name, icon in zip(("reroll", "reroll_highlighted", "reroll_locked"), icons):
        path = os.path.join(OUT_DIR, name + ".png")
        icon.save(path)
        print("wrote", os.path.relpath(path, REPO))
    if args.preview:
        preview(icons, args.preview)
        print("wrote", args.preview)


if __name__ == "__main__":
    main()
