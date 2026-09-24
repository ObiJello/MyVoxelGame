"""Vertical lamp screen that mirrors the 8x8 board.

The board lies flat (its lamps are 16 blocks apart on level 5), so it cannot
be watched from a control panel. This module taps every pixel's body and
food signals, carries them north over the board and up torch ladders, and
drives a wall of redstone lamps east of the board that a viewing deck looks
at. Everything is vanilla redstone; the levels below refer to the machine's
wire level (build.Y = 0), i.e. seven above the board's top.

Per pixel (i, j) (x0 = 16 i, z0 = 16 j on the board):

  body tap   dust on top of the body lamp (x0+10, 6, z0+12); the lamp is
             strongly powered by the latch torch TB, so the dust reads it.
             Two steps north reach level 8, a jog along z0+10 reaches the
             pixel's own column xb = 16 i + 2 j, seven more steps north
             reach level 15 and the line runs north at level 15.
  food tap   dust beside the FOOD output block (x0+15, 4, z0+12), which
             the FOOD torch powers strongly; four steps north reach level
             8, a jog along z0+8 reaches xf = xb + 1, two more steps reach
             level 10 and the line runs north at level 10.
  crossings  a line passes over the jogs of the pixels north of it (level
             8) with its support block right above the jog's dust, and the
             level-15 lines pass over the level-10 lines the same way, so
             nothing connects. The two line levels are chosen so that no
             climb passes a line's level next to a line that exists there:
             the food tap's climb (levels 5..8) at x0+15 sits beside body
             lines at 15, the body climb through level 10 at xb sits beside
             food lines that only start further north.
  ladders    the body lines of pixel column i end at the plane z = P(i)
             (P(i) = WZ0 + 5 (7-i) + 1), the food lines two planes further
             north (P(i)-2, after climbing to the ladder's level and a jog
             two cells west). A torch ladder (block, wall torch on its
             side, block above the torch, ...) is two cells wide and packs
             at pitch 2; the body ladder (xb, xb+1) starts at level 16, the
             food ladder (xb-1, xb) at 12 (odd rows) or 14 (even rows), all
             above every passing line. The body ladder's odd stage count
             leaves NOT(body) on its top block and the wall end inverts;
             the food ladder's even count leaves food and its end does not.
  links      dust on the ladder's top block runs east at height h(j)+1
             (body, h(j) = BASE + 4 (7-j)) or h(j)+2 (food) to the wall,
             passing over the shorter ladders of the pixels below it.
  wall       lamps at x = WX. Pixel (i, j) is 4 lamps wide (z = P-2..P+1)
             and 3 tall (h..h+2). The body link ends in a block, a wall
             torch (the inversion) and a repeater that strongly powers the
             centre lamp (P, h+1): a strongly powered lamp lights its four
             neighbours, so a body cell shows as a plus. The food link
             drives the top-north lamp (P-2, h+2) the same way, which shows
             as a small L. Black concrete fills the gaps between pixels so
             a strongly powered edge lamp cannot light the next pixel.

The viewer stands east of the wall (x > WX) looking west: board x runs to
the viewer's right (north), board y runs downwards (row 0 at the top), so
the N/E/S/W buttons are up/right/down/left on the screen.
"""
import build
from pixel import W as PW

N = 8
WX = 136                 # lamp plane; 133..135 hold each link's end block, torch and repeater
WZ0 = -60                # south edge of the wall's first pixel column (board column 7)
BASE = 17                # height of the lowest pixel row (board row 7); must be odd (ladder parity)
LV_JOG, LV_BN, LV_FN = 8, 15, 10              # jog level, body line level, food line level
DARK = 'minecraft:black_concrete'
FRAME = 'minecraft:polished_blackstone'
OPP = {'north': 'south', 'south': 'north', 'east': 'west', 'west': 'east'}


def plane(i):
    return WZ0 + 5 * (7 - i) + 1


def height(j):
    return BASE + 4 * (7 - j)


def lamp_cells(i, j):
    """(centre, corner) lamp cells of pixel (i, j) on the wall."""
    P, h = plane(i), height(j)
    return (WX, h + 1, P), (WX, h + 2, P - 2)


def wall_span():
    return (plane(7) - 2, plane(0) + 1), (BASE, height(0) + 2)


# ── primitives ───────────────────────────────────────────────────────────

def _sup(b, x, y, z):
    if b.w.blocks.get((x, y, z)) is None:
        b.solid(x, y, z)


def run_path(b, cells, rep_every=12):
    """Dust along `cells` (in signal order; consecutive cells adjacent or
    diagonal by one level). A repeater replaces a dust cell at most
    `rep_every` cells apart, only on a straight flat stretch, facing its
    input; a repeater is also placed on the last flat cell before a stretch
    of climbs that would otherwise run the power out. Every cell gets a
    support block under it."""
    n = len(cells)
    flat = [False] * n
    for k in range(1, n - 1):
        (px, py, pz), (x, y, z), (nx, ny, nz) = cells[k - 1], cells[k], cells[k + 1]
        flat[k] = py == y == ny and ((px == x == nx and pz != nz) or (pz == z == nz and px != nx))
    next_flat = [n] * n                       # index of the next flat cell after k (n if none)
    nf = n
    for k in range(n - 1, -1, -1):
        next_flat[k] = nf
        if flat[k]:
            nf = k
    since = 0
    for k, (x, y, z) in enumerate(cells):
        _sup(b, x, y - 1, z)
        if flat[k] and not build.PLUS and (since >= rep_every or since + (next_flat[k] - k) >= 14):
            px, pz = cells[k - 1][0], cells[k - 1][2]
            if px < x: facing = 'west'
            elif px > x: facing = 'east'
            elif pz < z: facing = 'north'
            else: facing = 'south'
            b.repeater(x, y, z, facing); since = 0
        else:
            assert since < 15 or build.PLUS, ('power runs out', cells[0], k, cells[k])
            b.wire(x, y, z); since += 1


def ladder_x(b, xa, xb, y0, ytop, z):
    """Torch ladder in the plane z between the x cells xa (stage-0 block)
    and xb: stage k has its block at (xa or xb, y0+k) and a wall torch on
    the other cell, which powers the next stage's block above it. The
    caller powers the stage-0 block. Returns the x of the top block; its
    value is signal XOR ((ytop - y0) mod 2)."""
    cells = (xa, xb)
    for k in range(ytop - y0 + 1):
        y = y0 + k
        bx, tx = cells[k % 2], cells[1 - k % 2]
        b.solid(bx, y, z)
        if y < ytop:
            b.wtorch(tx, y, z, 'east' if tx > bx else 'west')
    return cells[(ytop - y0) % 2]


def ladder_z(b, x, za, zb, y0, ytop):
    """Same, zigzagging between the z cells za (stage-0 block) and zb."""
    cells = (za, zb)
    for k in range(ytop - y0 + 1):
        y = y0 + k
        bz, tz = cells[k % 2], cells[1 - k % 2]
        b.solid(x, y, bz)
        if y < ytop:
            b.wtorch(x, y, tz, 'south' if tz > bz else 'north')
    return cells[(ytop - y0) % 2]


def torch_drop_west(b, x0, y0, z, steps):
    """Carries a signal down two levels per cell, westwards: the dust D_k at
    (x0-k, y0-2k) powers the block under it, a wall torch on that block's
    west face powers the dust right below the torch. The caller's line
    ends at D_0 (laid here); the last dust is at (x0-steps, y0-2 steps).
    Inverts once per step."""
    for k in range(steps):
        x, y = x0 - k, y0 - 2 * k
        b.wire(x, y, z)
        b.solid(x, y - 1, z)
        b.wtorch(x - 1, y - 1, z, 'west')
    b.wire(x0 - steps, y0 - 2 * steps, z)


def wall_end(b, y, z, invert=True):
    """Link end. Inverting: the dust at (WX-4, y, z) points into a block, a
    wall torch on the block's east face inverts, a repeater strongly powers
    the lamp. Non-inverting: the dust at (WX-3) points into a block that the
    repeater reads."""
    if invert:
        b.solid(WX - 3, y, z)
        b.wtorch(WX - 2, y, z, 'east')
    else:
        b.solid(WX - 2, y, z)
    b.repeater(WX - 1, y, z, 'west')


# ── the screen ───────────────────────────────────────────────────────────

def body_cells(i, j):
    """(path cells from the tap to the ladder foot, link cells, cover blocks)
    of pixel (i, j)'s body chain."""
    x0, z0 = PW * i, PW * j
    P, h = plane(i), height(j)
    xb = x0 + 2 * j
    cells = [(x0 + 10, 6, z0 + 12), (x0 + 10, 7, z0 + 11), (x0 + 10, LV_JOG, z0 + 10)]
    covers = []
    if j > 0:
        step = -1 if xb < x0 + 10 else 1
        cells += [(x, LV_JOG, z0 + 10) for x in range(x0 + 10 + step, xb + step, step)]
        cells += [(xb, LV_JOG + k, z0 + 10 - k) for k in range(1, LV_BN - LV_JOG + 1)]    # climb to 15 at z0+3
        z_line = z0 + 10 - (LV_BN - LV_JOG) - 1
    else:
        # row 0: the column x0 cannot climb through the food level next to the previous
        # column's row-7 food line, so the climb goes south along x0+1 (a food column
        # that only starts further north), steps west at level 14 and turns north
        cells += [(x, LV_JOG, z0 + 10) for x in range(x0 + 9, x0, -1)]
        cells += [(x0 + 1, LV_JOG, z0 + 11), (x0 + 1, LV_JOG, z0 + 12)]
        cells += [(x0 + 1, LV_JOG + k, z0 + 12 + k) for k in range(1, 7)]                 # 9..14 at z0+13..z0+18
        cells += [(x0, 14, z0 + 18), (x0, LV_BN, z0 + 17)]
        covers = [(x0 + 1, LV_BN, z0 + 18)]                # keeps (x0+1, 14) apart from the row-1 body line at x0+2
        z_line = z0 + 16
    cells += [(xb, LV_BN, z) for z in range(z_line, P + 1, -1)]          # north to P+2
    cells += [(xb, 16, P + 1)]                                            # points north into the stage-0 block
    xt = xb + ((h - 16) % 2)                                              # the ladder's top block (xb+1)
    link = [(x, h + 1, P) for x in range(xt, WX - 3)]                      # dust on the top block, east to WX-4
    return cells, link, covers


def food_ladder(i, j):
    """(west cell, first level) of the food ladder in plane P(i)-2. The ladder
    occupies (xb-1, xb) with its first block on the east cell xb = xf-1, one
    cell west of the line, so the ladder of row 7 stays clear of the next
    column's lines and the link on the top block (also the east cell) is
    never beside the taller ladder of the row above. Odd rows climb to 12
    and jog at 12, even rows to 14, so the jogs of neighbouring rows (which
    overlap by one cell) sit two levels apart and both stay below the body
    ladder's first torch (level 16). The stage count to h+1 is even, so the
    top block carries the signal itself and the wall end does not invert."""
    xf = PW * i + 2 * j + 1
    return xf - 2, (12 if j % 2 == 1 else 14)


def food_cells(i, j):
    x0, z0 = PW * i, PW * j
    P, h = plane(i), height(j)
    xf = x0 + 2 * j + 1
    xl, y0 = food_ladder(i, j)
    cells = [(x0 + 15, 4 + k, z0 + 12 - k) for k in range(0, 5)]           # tap, climb to level 8 at z0+8
    cells += [(x, LV_JOG, z0 + 8) for x in range(x0 + 14, xf - 1, -1)]   # jog west (empty for j = 7)
    cells += [(xf, 9, z0 + 7), (xf, LV_FN, z0 + 6)]
    z_end = P + (y0 - LV_FN)                                              # P+2 (odd rows) / P+4 (even rows)
    cells += [(xf, LV_FN, z) for z in range(z0 + 5, z_end - 1, -1)]      # north to z_end
    cells += [(xf, LV_FN + k, z_end - k) for k in range(1, y0 - LV_FN + 1)]   # climb to y0 at z = P
    cells += [(xf - 1, y0, P), (xf - 1, y0, P - 1)]                     # one cell west, one north: points into the stage-0 block
    xt = (xl + 1) - ((h + 1 - y0) % 2)                                    # the ladder's top block (xl+1: even stage count)
    link = [(x, h + 2, P - 2) for x in range(xt, WX - 2)]                 # east to WX-3 (non-inverting end)
    return cells, link


def body_chain(b, i, j):
    P, h = plane(i), height(j)
    xb = PW * i + 2 * j
    cells, link, covers = body_cells(i, j)
    run_path(b, cells)
    for c in covers:
        b.solid(*c)
    xt = ladder_x(b, xb, xb + 1, 16, h, P)                                # top block holds NOT(body)
    assert xt == link[0][0]
    run_path(b, link)
    wall_end(b, h + 1, P)


def food_chain(b, i, j):
    P, h = plane(i), height(j)
    xf = PW * i + 2 * j + 1
    cells, link = food_cells(i, j)
    run_path(b, cells)
    xl, y0 = food_ladder(i, j)
    xt = ladder_x(b, xl + 1, xl, y0, h + 1, P - 2)                        # first block on the east cell; even stage count: top block holds food
    assert xt == link[0][0]
    run_path(b, link)
    wall_end(b, h + 2, P - 2, invert=False)


def wall(b):
    (z0, z1), (y0, y1) = wall_span()
    for z in range(z0, z1 + 1):
        for y in range(y0, y1 + 1):
            b.solid(WX, y, z, DARK)
    for z in range(z0 - 1, z1 + 2):
        b.solid(WX, y0 - 1, z, FRAME); b.solid(WX, y1 + 1, z, FRAME)
    for y in range(y0 - 1, y1 + 2):
        b.solid(WX, y, z0 - 1, FRAME); b.solid(WX, y, z1 + 1, FRAME)
    lamps = {}
    for i in range(N):
        for j in range(N):
            P, h = plane(i), height(j)
            for z in range(P - 2, P + 2):
                for y in range(h, h + 3):
                    b.lamp(WX, y, z)
            lamps[(i, j)] = lamp_cells(i, j)
    return lamps


def screen(b):
    """Lays everything. Returns {'body': {(i,j): lamp}, 'food': {(i,j): lamp}}."""
    lamps = wall(b)
    for i in range(N):
        for j in range(N):
            body_chain(b, i, j)
            food_chain(b, i, j)
    return {'body': {k: v[0] for k, v in lamps.items()}, 'food': {k: v[1] for k, v in lamps.items()}}
