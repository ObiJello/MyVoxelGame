#!/usr/bin/env python3
"""Render the game's pre-rendered app icon: the TNT block.

TNT is the default icon (the launcher's Settings -> App icon offers Random and
every other full block too). Those others are drawn when the game starts, by
src/client/renderer/gui/BlockIcon.cpp; TNT is the one the bundle / exe ships
already drawn, so Finder, Explorer and a launch without --icon show it. The
two must draw the same thing: the constants below are BlockIcon.cpp's.

Follows Minecraft's own game icon (the grass block in the asset index's
icons/minecraft.icns and icons/icon_*.png):
  * no background — the block alone on transparency;
  * a true isometric cube (a regular hexagon: width = sqrt(3)/2 x height)
    filling the full height of the square, so it is 86.6 % as wide;
  * the top face at full brightness, the left face a little dimmer, the
    right face in deep shadow;
  * a thin light rim along the top face's two front edges.
The model's north face is on the left and its west face on the right, as in
an inventory slot. Each texel is its own polygon, rendered 4x oversized and
filtered down, so the pixel art stays crisp and the silhouette anti-aliased.

Outputs, under src/platform/icon/ (outside assets/, so it is not staged into
the game's asset tree):
  AppIcon.icns  macOS bundle icon, 16..1024 px.
  AppIcon.ico   Windows exe + window icon, 16..256 px.
  window.png    256 px, for glfwSetWindowIcon on Linux (staged as
                assets/game_icon.png).

Usage: python3 tools/make_game_icon.py
Needs Pillow; the .icns step needs macOS `iconutil`.
"""
import math
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
TEXTURES = ROOT / "assets" / "textures" / "block"
OUT = ROOT / "src" / "platform" / "icon"

SS = 4  # supersampling factor

# The texture on each visible face and its model uv rotation (degrees
# clockwise, as in the block model JSON): TNT is cube_bottom_top.
FACES = {
    "top":   ("tnt_top", 0),
    "left":  ("tnt_side", 0),   # north
    "right": ("tnt_side", 0),   # west
}

# Face brightness, measured off minecraft.icns: the left face keeps ~0.92 of
# the top's brightness and the right face drops to ~0.45 (its dirt and grass
# land between 0.3 and 0.46 of the left's).
SHADE = {"top": 1.0, "left": 0.92, "right": 0.45}
# The top face's front-edge rim, also off minecraft.icns: ~0.85 % of the icon
# height thick, the top's colour lifted 40 % toward white.
RIM_THICKNESS = 0.0085
RIM_LIFT = 0.4


def load_face(name, rotation):
    img = Image.open(TEXTURES / f"{name}.png").convert("RGBA")
    # Animated textures are a vertical strip of square frames: take frame 0.
    if img.height > img.width:
        img = img.crop((0, 0, img.width, img.width))
    if rotation:
        # Model uv rotation is clockwise; PIL rotates counter-clockwise.
        img = img.rotate(-rotation, expand=False)
    return img


def shade(rgba, k):
    r, g, b, a = rgba
    return (round(r * k), round(g * k), round(b * k), a)


def render(faces, canvas):
    """The block alone on a transparent `canvas` px square."""
    big = canvas * SS
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # Full height less one final pixel at each end (the anti-aliased tips
    # would otherwise be cut by the square). s is the projected edge length.
    s = (big - 2 * SS) / 2.0
    hw = s * math.sqrt(3) / 2.0
    cx, y0 = big / 2.0, float(SS)

    # Top-face corners: far = SE, left = NE, right = SW, near = NW.
    left = (cx - hw, y0 + s / 2)
    right = (cx + hw, y0 + s / 2)
    near = (cx, y0 + s)

    def quad(origin, du, dv, tex, k, rim_edges=()):
        n = tex.width
        px = tex.load()
        rim = RIM_THICKNESS * big
        for y in range(n):
            for x in range(n):
                c = px[x, y]
                if c[3] == 0:
                    continue
                u0, u1, v0, v1 = x / n, (x + 1) / n, y / n, (y + 1) / n
                pts = [(origin[0] + du[0] * u + dv[0] * v, origin[1] + du[1] * u + dv[1] * v)
                       for u, v in ((u0, v0), (u1, v0), (u1, v1), (u0, v1))]
                draw.polygon(pts, fill=shade(c, k))
        # The rim: a strip inside the face along each named edge, in the
        # face's own texels lifted toward white.
        for a, b in rim_edges:
            ex, ey = b[0] - a[0], b[1] - a[1]
            length = math.hypot(ex, ey)
            # Inward normal: toward the face's far corner (up the screen).
            nx, ny = ey / length, -ex / length
            if ny > 0:
                nx, ny = -nx, -ny
            strip = Image.new("L", img.size, 0)
            ImageDraw.Draw(strip).polygon(
                [a, b, (b[0] + nx * rim, b[1] + ny * rim), (a[0] + nx * rim, a[1] + ny * rim)], fill=255)
            lit = Image.blend(img, Image.new("RGBA", img.size, (255, 255, 255, 255)), RIM_LIFT)
            img.paste(lit, (0, 0), strip)

    # Top: texture (0,0) at NW (near), +u toward east (NE = left), +v toward
    # south (SW = right). Its two front edges, near-left and near-right,
    # carry the rim.
    quad(near, (left[0] - near[0], left[1] - near[1]), (right[0] - near[0], right[1] - near[1]),
         faces["top"], SHADE["top"], rim_edges=((left, near), (near, right)))
    # North (left) face seen from outside: u runs NE -> NW, v runs down.
    quad(left, (near[0] - left[0], near[1] - left[1]), (0, s), faces["left"], SHADE["left"])
    # West (right) face seen from outside: u runs NW -> SW, v runs down.
    quad(near, (right[0] - near[0], right[1] - near[1]), (0, s), faces["right"], SHADE["right"])

    return img.resize((canvas, canvas), Image.LANCZOS)


def write_icns(icon1024, dest):
    if not shutil.which("iconutil"):
        print("  iconutil not found (not macOS?) — skipping AppIcon.icns")
        return
    with tempfile.TemporaryDirectory() as tmp:
        iconset = Path(tmp) / "AppIcon.iconset"
        iconset.mkdir()
        for pt in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = pt * scale
                name = f"icon_{pt}x{pt}{'@2x' if scale == 2 else ''}.png"
                icon1024.resize((px, px), Image.LANCZOS).save(iconset / name)
        subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(dest)], check=True)


def main():
    faces = {k: load_face(*FACES[k]) for k in ("top", "left", "right")}
    OUT.mkdir(parents=True, exist_ok=True)
    icon = render(faces, 1024)
    write_icns(icon, OUT / "AppIcon.icns")
    icon256 = icon.resize((256, 256), Image.LANCZOS)
    icon256.save(OUT / "AppIcon.ico", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    icon256.save(OUT / "window.png")
    print(f"  {OUT.relative_to(ROOT)}/{{AppIcon.icns, AppIcon.ico, window.png}}")


if __name__ == "__main__":
    sys.exit(main())
