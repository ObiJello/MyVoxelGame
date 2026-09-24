"""Dense board pixel for Snake v2: a 16x16 cell, seven levels high.

Levels (relative to the wire level Y = 0):
  -1  floor
   0  COLUMN lines, north->south, active-low: HX_L at x0, TX_L at x0+5,
      FX_L at x0+10; and the column branches that climb to level 2
   1  glowstone under the rows (rows never talk to the columns), solids
      under the branch climbs
   2  ROW lines, west->east, active-low: HYS_L (set) at z0, HYC_L (check)
      at z0+8, TYE_L (erase) at z0+10, FY_L (food) at z0+14. Every NOR
      block is a TAP beside its row: a repeater reads the row cell and
      points into the block; the column input arrives through a second
      repeater. All inputs through repeaters, so nothing leaks between
      lines and the row itself is never interrupted.
   3  torches on top of the NOR blocks
   4  the RS latch (blocks A and B on the SET and ERASE torches), the
      collision logic (block C4 on the CHECK torch, block D), the
      collision OR line COL (active-high, north->south) at x0+2
   5  lamps: body lamp above TB, food lamp above the FOOD output block
   6  the latch's return loop (TB back to A) over everything

NOR blocks:  SET   (x0+4,  2, z0+2)  = NOR(HYS_L, HX_L)   torch -> A
             CHECK (x0+4,  2, z0+6)  = NOR(HYC_L, HX_L)   torch -> C4
             ERASE (x0+9,  2, z0+12) = NOR(TYE_L, TX_L)   torch -> B
             FOOD  (x0+14, 2, z0+12) = NOR(FY_L,  FX_L)   torch -> lamp
The HX branch at z0+4 feeds SET from the south and CHECK from the north.
COLLIDE = HX & HYC & Q is formed at level 4: C4 is powered by the CHECK
torch, its side torch (NOT that) and a Q_L branch off the A->B dust feed
block D, and D's torch drives the OR line.

Board interface: `through_columns` / `through_rows` lay the line cells
(with one repeater per pixel) skipping cells the pixels own, so lay the
pixels first.
"""
from build import Y

W = 16                      # pixel pitch, both axes
COL_X = (0, 5, 10)          # HX_L, TX_L, FX_L column offsets (level 0)
OR_X = 2                    # collision OR line (level 4)
ROW_Z = (0, 8, 10, 14)      # HYS_L, HYC_L, TYE_L, FY_L row offsets (level 2)
COL_REP = {0: 3, 5: 11, 10: 11}   # column repeater z offset: right before each branch
OR_REP = 12                       # OR line repeater z offset
ROW_REP = {0: 12, 8: 12, 10: 8, 14: 6}    # row repeater x offset: 8 cells before each tap
L0, L1, L2, L3, L4, L5, L6 = 0, 1, 2, 3, 4, 5, 6


def through_columns(b, x0, z0, z_from, z_to):
    """Column dust at level 0 from z_from to z_to (inclusive, flowing
    south) for the three select columns of the pixel column at x0, and the
    OR line at level 4; a repeater per pixel pitch, phased from z0."""
    for cx in COL_X:
        for z in range(z_from, z_to + 1):
            if (z - z0) % W == COL_REP[cx]:
                b.repeater(x0 + cx, L0, z, 'north')
            else:
                b.wire(x0 + cx, L0, z)
    for z in range(z_from, z_to + 1):
        if b.w.blocks.get((x0 + OR_X, L3, z)) is None:
            b.solid(x0 + OR_X, L3, z)
        if (z - z0) % W == OR_REP:
            b.repeater(x0 + OR_X, L4, z, 'north')
        else:
            b.wire(x0 + OR_X, L4, z)


def through_rows(b, x0, z0, x_from, x_to):
    """Row dust at level 2 on glowstone from x_from to x_to (inclusive,
    flowing east) for the four rows of the pixel row at z0, skipping cells
    a pixel already owns; a repeater per pixel pitch, phased from x0."""
    for rz in ROW_Z:
        z = z0 + rz
        for x in range(x_from, x_to + 1):
            if b.w.blocks.get((x, L1, z)) is None:
                b.glow(x, L1, z)
            if b.w.blocks.get((x, L2, z)) is not None:
                continue
            if (x - x0) % W == ROW_REP[rz]:
                b.repeater(x, L2, z, 'west')
            else:
                b.wire(x, L2, z)


def _climb(b, cx, z, x_end):
    """Column branch: a two step climb straight off the column cell
    (cx, 0, z) eastwards, then level-2 dust to x_end (inclusive)."""
    b.solid(cx + 1, L0, z); b.wire(cx + 1, L1, z)
    b.solid(cx + 2, L1, z); b.wire(cx + 2, L2, z)
    for x in range(cx + 3, x_end + 1):
        b.solid(x, L1, z); b.wire(x, L2, z)


def pixel(b, x0, z0):
    """Lays one pixel. Returns the lamp positions and a few probe points."""
    hx, tx, fx = (x0 + c for c in COL_X)
    zs, zc, ze, zf = (z0 + r for r in ROW_Z)

    # ── SET (block z0+2) and CHECK (block z0+6) share the HX branch at z0+4
    _climb(b, hx, z0 + 4, x0 + 4)                        # dust ends at (x0+4, 2, z0+4)
    b.repeater(x0 + 4, L2, zs + 1, 'north')              # row tap: reads (x0+4, 2, zs)
    b.solid(x0 + 4, L2, zs + 2)                          # SET block
    b.repeater(x0 + 4, L2, zs + 3, 'south')              # HX from the branch
    b.torch(x0 + 4, L3, zs + 2)                          # SET torch -> A above
    b.repeater(x0 + 4, L2, zc - 1, 'south')              # row tap: reads (x0+4, 2, zc)
    b.solid(x0 + 4, L2, zc - 2)                          # CHECK block
    b.repeater(x0 + 4, L2, zc - 3, 'north')              # HX from the branch
    b.torch(x0 + 4, L3, zc - 2)                          # CHECK torch -> C4 above

    # ── ERASE: block (x0+9, 2, z0+12); TX climbs at x0+6/7, repeater at x0+8
    bz = ze + 2
    _climb(b, tx, bz, tx + 2)
    b.repeater(x0 + 8, L2, bz, 'west')                   # TX into the block from the west
    b.solid(x0 + 9, L2, bz)                              # ERASE block
    b.repeater(x0 + 9, L2, bz - 1, 'north')              # row tap: reads (x0+9, 2, ze)
    b.torch(x0 + 9, L3, bz)                              # ERASE torch -> B above

    # ── FOOD: block (x0+14, 2, z0+12); FX climbs at x0+11/12, repeater at x0+13
    _climb(b, fx, bz, fx + 2)
    b.repeater(x0 + 13, L2, bz, 'west')
    b.solid(x0 + 14, L2, bz)                             # FOOD block
    b.repeater(x0 + 14, L2, zf - 1, 'south')             # row tap: reads (x0+14, 2, zf)
    b.torch(x0 + 14, L3, bz)
    b.solid(x0 + 14, L4, bz)
    b.lamp(x0 + 14, L5, bz)

    # ── RS latch, level 4: A (x0+4, z0+2) on the SET torch, B (x0+9, z0+12) on the ERASE torch
    ax, az = x0 + 4, zs + 2
    bx = x0 + 9
    b.solid(ax, L4, az)                                  # A
    b.wtorch(ax + 1, L4, az, 'east')                     # TA: lit <=> Q = 0 (its net is Q_L)
    for x in range(ax + 2, bx + 1):                      # east along az to x = bx
        b.solid(x, L3, az); b.wire(x, L4, az)
    for z in range(az + 1, bz):                          # south along bx into B's north face
        b.solid(bx, L3, z); b.wire(bx, L4, z)
    b.solid(bx, L4, bz)                                  # B
    b.wtorch(bx + 1, L4, bz, 'east')                     # TB: lit <=> Q = 1
    b.lamp(bx + 1, L5, bz)                               # body lamp
    # return loop: TB -> (bx+2, 4, bz), climb two steps to level 6, west
    # along bz+2, north along x0+6 (repeater half way), then down to
    # (x0+4, 4, az+2) and (x0+4, 4, az+1), which points north into A.
    b.solid(bx + 2, L3, bz); b.wire(bx + 2, L4, bz)
    b.solid(bx + 2, L4, bz + 1); b.wire(bx + 2, L5, bz + 1)
    b.glow(bx + 2, L5, bz + 2); b.wire(bx + 2, L6, bz + 2)   # glowstone: the CLEAR line (board.clear_lines)
    for x in range(bx + 1, x0 + 5, -1):                       # runs one level below; a solid here would
        b.glow(x, L5, bz + 2); b.wire(x, L6, bz + 2)          # carry Q down into it
    for z in range(bz + 1, az + 1, -1):
        b.solid(x0 + 6, L5, z)
        if z == az + 7:
            b.repeater(x0 + 6, L6, z, 'south')           # input from the south (flow north)
        else:
            b.wire(x0 + 6, L6, z)
    b.solid(x0 + 5, L4, az + 2); b.wire(x0 + 5, L5, az + 2)
    b.solid(x0 + 4, L3, az + 2); b.wire(x0 + 4, L4, az + 2)
    b.solid(ax, L3, az + 1); b.wire(ax, L4, az + 1)      # points north into A

    # ── collision, level 4: C4 (x0+4, z0+6) on the CHECK torch; its side
    #    torch (NOT(HX & HYC)) and a Q_L branch feed D (x0+5, z0+8); D's
    #    torch reports HX & HYC & Q onto the OR line at x0+2
    cz = zc - 2
    b.solid(x0 + 4, L4, cz)                              # C4
    b.wtorch(x0 + 5, L4, cz, 'east')
    b.solid(x0 + 5, L3, cz + 1); b.wire(x0 + 5, L4, cz + 1)      # -> D from the north
    b.solid(x0 + 5, L4, cz + 2)                          # D
    for x in (bx - 1, bx - 2, bx - 3):                   # Q_L: off (bx, 4, cz+2) west into D
        b.solid(x, L3, cz + 2); b.wire(x, L4, cz + 2)
    b.wtorch(x0 + 4, L4, cz + 2, 'west')                 # COLLIDE
    b.solid(x0 + 3, L3, cz + 2); b.wire(x0 + 3, L4, cz + 2)      # -> OR line

    return {
        'body_lamp': (bx + 1, L5, bz), 'food_lamp': (x0 + 14, L5, bz),
        'A': (ax, L4, az), 'B': (bx, L4, bz), 'TA': (ax + 1, L4, az), 'TB': (bx + 1, L4, bz),
        'col_join': (x0 + 3, L4, cz + 2),
    }
