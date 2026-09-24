"""Viewing deck with the control console, east of the lamp screen.

The deck is a floating floor (level FLOOR, walked on at FLOOR+1) with a
console desk in the middle of its west half: four stone buttons in a
D-pad (N/E/S/W = up/right/down/left on the screen), the start lever and,
set into the floor west of the desk, the death lamp.

A button (or the lever) strongly powers the desk block it stands on; dust
directly under that block (in the floor layer, hidden by the desk) picks
it up, runs north under the desk to a hole at the desk's north edge, drops
one level and runs north under the floor to the deck's edge. There a dust
staircase brings it down to level 16, a run north reaches the lane's own
row (z = -93 .. -101, one per signal) and a run west crosses the whole
machine at level 16 to the original control platform, where a torch drop
(two levels per cell, eight cells) lands on the button lane's first cells.
Eight torches keep the polarity.

The death signal comes the other way: the DEADFB lane passes the platform
row; the block under its cell there is powered by the dust, a wall torch
on that block inverts, a dust cell, a second block and torch invert again,
and a torch ladder climbs to level 16. The line runs east at level 17
along z = -91 (two rows north of the first button row) to the deck, south
under the floor, up a second ladder and through a repeater into the block
under the floor lamp.
"""
from screen import run_path, ladder_x, ladder_z, torch_drop_west

FLOOR = 21                         # eye level ~ the wall's middle; F-2-LEVEL_DEAD even (the DEAD ladder's top block = DEAD)
                                   # and the staircase down to LEVEL_LINE still ends north of the control rows
X0, X1 = 156, 190
Z0, Z1 = -72, -12
DESK = ((166, 176), (-52, -38))
LEVEL_LINE = 16                    # the control lines cross the machine at this level
LEVEL_DEAD = 17
FLOOR_BLOCK = 'minecraft:polished_deepslate'
EDGE_BLOCK = 'minecraft:polished_blackstone'
DESK_BLOCK = 'minecraft:polished_blackstone'
RAIL_BLOCK = 'minecraft:glass'
LIGHT_BLOCK = 'minecraft:sea_lantern'

# console layout: button/lever cell on the desk top (level FLOOR+2), the lane column under the
# floor it is wired to, and the row (z) of its line across the machine
CONTROLS = {
    'N':     ((167, -42), 166, -93),
    'E':     ((169, -44), 168, -95),
    'S':     ((171, -42), 170, -97),
    'W':     ((169, -40), 172, -99),
    'PAUSE': ((175, -42), 174, -101),
}
LAMP = (164, FLOOR, -44)
DEAD_X = 162


def _console_paths():
    """Level-FLOOR dust under the desk from each control to its hole, then the
    level FLOOR-1 cells to the lane column. Returns {name: (cells, hole)}."""
    F = FLOOR
    return {
        'N': ([(167, F, z) for z in range(-42, -46, -1)] + [(167, F - 1, -46), (166, F - 1, -46)], (167, F, -46)),
        'E': ([(169, F, -44), (169, F, -45), (169, F - 1, -46), (169, F - 1, -47), (169, F - 1, -48), (168, F - 1, -48)],
              (169, F, -46)),
        'S': ([(171, F, z) for z in range(-42, -46, -1)] + [(171, F - 1, z) for z in range(-46, -50, -1)] + [(170, F - 1, -49)],
              (171, F, -46)),
        'W': ([(x, F, -40) for x in range(169, 174)] + [(173, F, z) for z in range(-41, -46, -1)]
              + [(173, F - 1, z) for z in range(-46, -51, -1)] + [(172, F - 1, -50)], (173, F, -46)),
        'PAUSE': ([(175, F, z) for z in range(-42, -51, -1)] + [(175, F - 1, -51), (174, F - 1, -51)], (175, F, -51)),
    }


def deck(b, lane_x, deadfb_cell, origin=(0, 0)):
    """lane_x: {'N','E','S','W','PAUSE': x of the north-flowing lane at level 0 whose
    dust cells at z -93.. the lines land on}. deadfb_cell: (x, z) of a DEADFB
    dust cell on the platform row. Returns {'buttons', 'lever', 'lamp'}."""
    ox, oz = origin                        # the deck's constants are laid out at origin (0, 0); shift everything but the line rows
    X0_, X1_, Z0_, Z1_ = X0 + ox, X1 + ox, Z0 + oz, Z1 + oz
    desk = ((DESK[0][0] + ox, DESK[0][1] + ox), (DESK[1][0] + oz, DESK[1][1] + oz))
    controls = {n: ((cx + ox, cz + oz), col + ox, zrow) for n, ((cx, cz), col, zrow) in CONTROLS.items()}
    paths = {n: ([(x + ox, y, z + oz) for (x, y, z) in cells], (hx + ox, hy, hz + oz))
             for n, (cells, (hx, hy, hz)) in _console_paths().items()}
    lamp = (LAMP[0] + ox, LAMP[1], LAMP[2] + oz)
    dead_x = DEAD_X + ox
    F = FLOOR
    (dx0, dx1), (dz0, dz1) = desk
    holes = set()
    # ── console wiring, staircase, lines across the machine, drops onto the lanes
    for name, (cell, col, zrow) in controls.items():
        cells, hole = paths[name]
        holes.add(hole)
        assert cells[-1][0] == col, (name, cells[-1], col)
        z_last = cells[-1][2]
        cells = list(cells) + [(col, F - 1, z) for z in range(z_last - 1, Z0_ - 1, -1)]       # north under the floor to the edge
        cells += [(col, F - 1 - k, Z0_ - 1 - k) for k in range(0, F - 1 - LEVEL_LINE + 1)]     # staircase down to the line level
        z_line_start = Z0_ - 1 - (F - 1 - LEVEL_LINE)
        cells += [(col, LEVEL_LINE, z) for z in range(z_line_start - 1, zrow - 1, -1)]         # north to its row
        xd = lane_x[name]
        cells += [(x, LEVEL_LINE, zrow) for x in range(col - 1, xd + 8, -1)]                  # west to the drop
        run_path(b, cells)
        torch_drop_west(b, xd + 8, LEVEL_LINE, zrow, LEVEL_LINE // 2)                        # lands on (xd, 0, zrow)
    # ── the desk and its controls
    for x in range(dx0, dx1 + 1):
        for z in range(dz0, dz1 + 1):
            b.solid(x, F + 1, z, DESK_BLOCK)
    buttons = {}
    for name, (cell, col, zrow) in controls.items():
        x, z = cell
        if name == 'PAUSE':
            b.lever(x, F + 2, z, 'west')
            lever = (x, F + 2, z)
        else:
            b.button(x, F + 2, z, 'west')
            buttons[name] = (x, F + 2, z)
    # ── death lamp: tap at the platform, ladder, line east, ladder up under the deck
    tx, tz = deadfb_cell
    b.solid(tx, -1, tz)                            # under the DEADFB dust: powered by it
    b.wtorch(tx - 1, -1, tz, 'west')               # NOT DEAD
    b.wire(tx - 2, -1, tz)
    b.solid(tx - 3, -1, tz)                        # powered by the dust pointing into it
    b.wtorch(tx - 4, -1, tz, 'west')               # DEAD, powers the ladder's first block above it
    xt = ladder_x(b, tx - 4, tx - 5, 0, LEVEL_DEAD - 1, tz)      # even stage count: top block = DEAD
    line = [(xt, LEVEL_DEAD, tz), (xt + 1, LEVEL_DEAD, tz), (xt + 1, LEVEL_DEAD, tz + 1)]
    line += [(x, LEVEL_DEAD, tz + 1) for x in range(xt + 2, dead_x + 1)]
    line += [(dead_x, LEVEL_DEAD, z) for z in range(tz + 2, lamp[2])]       # south to the cell before the ladder block
    run_path(b, line)
    zt = ladder_z(b, dead_x, lamp[2], lamp[2] + 1, LEVEL_DEAD, F - 2)         # top block (F-2) = DEAD
    b.wire(dead_x, F - 1, zt)
    b.repeater(dead_x + 1, F - 1, zt, 'west')
    b.solid(dead_x + 2, F - 1, zt)
    b.lamp(*lamp)
    assert (dead_x + 2, F, zt) == lamp, (zt, lamp)
    # ── the floor (only where nothing else is), pillars, railing, lights
    for x in range(X0_, X1_ + 1):
        for z in range(Z0_, Z1_ + 1):
            if (x, F, z) in holes or b.w.blocks.get((x, F, z)) is not None:
                continue
            edge = x in (X0_, X1_) or z in (Z0_, Z1_)
            b.solid(x, F, z, EDGE_BLOCK if edge else FLOOR_BLOCK)
    for x in (X0_ + 2, X1_ - 2):
        for z in (Z0_ + 2, (Z0_ + Z1_) // 2, Z1_ - 2):
            for y in range(-1, F):
                b.solid(x, y, z, EDGE_BLOCK)
    for x in range(X0_, X1_ + 1):
        for z in (Z0_, Z1_):
            b.solid(x, F + 1, z, RAIL_BLOCK)
    for z in range(Z0_, Z1_ + 1):
        b.solid(X1_, F + 1, z, RAIL_BLOCK)
        if z % 6 == 0:
            b.solid(X0_, F + 1, z, RAIL_BLOCK)        # a low, open railing on the screen side
    for x in (X0_ + 4, (X0_ + X1_) // 2, X1_ - 4):
        for z in (Z0_ + 6, Z1_ - 6):
            blk = b.w.blocks.get((x, F, z))
            if blk is not None and blk.kind == 'solid':
                b.solid(x, F, z, LIGHT_BLOCK)
    # spawn between the west rail and the desk, looking west down onto the panel (the desk behind)
    return {'buttons': buttons, 'lever': lever, 'lamp': lamp, 'spawn': (X0_ + 5, F + 1, (dz0 + dz1) // 2)}
