"""One board pixel of the Snake machine (32 x 32 footprint, origin cx, cz).

Through-signals:
  floor lanes (north->south)   x = cx+0  HX_L   head column select, active low
                               x = cx+3  TX_L   tail column select
                               x = cx+6  FX_L   food column select
                               x = cx+27 DEAD   collector (this cell joins it)
  upper rows (west->east)      z = cz+0  HYS_L  NOT(head row j & PULSE_SET)
                               z = cz+8  HYC_L  NOT(head row j & PULSE_CHECK)
                               z = cz+16 TYE_L  NOT(tail row j & PULSE_ERASE)
                               z = cz+24 FY_L   food row select
Inside: four NOR gates in a column at x = cx+12 (one per row band), an RS
latch of two torches at z = cz+28 (Q = body), a body lamp under the latch
loop at (cx+21, cz+30) and a food lamp at (cx+15, cz+27). Internal floor
lanes: cx+9 (Q_L, flows north to the collision gate), cx+16 (SET pulse down
to the latch), cx+24 (RESET pulse down to the latch).
"""
from build import Builder, Y
from cells import run_wire
from pcb import Pcb, UP, MID

W = 32   # cell pitch in both axes


def cell(b, cx, cz):
    p = Pcb(b)
    X = lambda x: cx + x
    Z = lambda z: cz + z

    def upper(x, z): p.upper_wire(X(x), Z(z))
    def upper_on_solid(x, z):
        b.solid(X(x), MID, Z(z)); b.wire(X(x), UP, Z(z))
    def mid_on_solid(x, z):
        b.solid(X(x), Y, Z(z)); b.wire(X(x), MID, Z(z))

    # ── gate column: block (12, zb), branch from the row above, torch east ──
    def gate(zrow, zb):
        upper(12, zrow + 1); upper(12, zrow + 2)          # branch off the global row
        b.glow(X(12), MID, Z(zb)); b.solid(X(12), UP, Z(zb))
        b.glow(X(13), MID, Z(zb)); b.wtorch(X(13), UP, Z(zb), 'east')
    # west-face rows from the lane vias (via_up: ramp x+1..x+3, row from x+4)
    def lane_row(lx, z, x_end):
        start = p.via_up(X(lx), Z(z))
        for x in range(start[0], X(x_end) + 1):
            p.upper_wire(x, Z(z))

    # band 0: SET = NOR(HX_L, HYS_L)
    gate(0, 3); lane_row(0, 3, 11)
    upper_on_solid(14, 3)                                  # torch out on a solid (feeds the ramp)
    mid_on_solid(15, 3)                                    # west-arrival ramp down to lane 16
    run_wire(b, [(X(16), Z(3)), (X(16), Z(26))], rep_every=8)
    p.via_up(X(16), Z(26))                                 # -> (18,26) upper
    upper(18, 27)                                          # points south into A (18,28)

    # band 1: COLLIDE = NOR(HX_L, HYC_L, Q_L)
    gate(8, 11); lane_row(0, 11, 11)
    # Q_L arrives from lane 9 (flowing north) through a via at z=13 -> (11,13),(12,13),(12,12)
    b.solid(X(10), Y, Z(13)); b.wire(X(10), MID, Z(13)); upper(11, 13); upper(12, 13); upper(12, 12)
    # torch out east over lanes 16 and 24 to the DEAD collector lane at 27
    for x in range(14, 25):
        if x == 20: p.upper_repeater(X(20), Z(11), 'west')
        else: upper(x, 11)
    upper_on_solid(25, 11); mid_on_solid(26, 11)           # west-arrival ramp down onto lane 27
    b.wire(X(27), Y, Z(11))                                # the collector cell (lane built by caller)

    # band 2: RESET = NOR(TX_L, TYE_L)
    gate(16, 19); lane_row(3, 19, 11)
    for x in range(14, 22):
        if x == 18: p.upper_repeater(X(18), Z(19), 'west')
        else: upper(x, 19)
    upper_on_solid(22, 19); mid_on_solid(23, 19)           # ramp down onto lane 24
    run_wire(b, [(X(24), Z(19)), (X(24), Z(26))], rep_every=4)
    mid_on_solid(23, 26); upper(22, 26); upper(22, 27)     # west-side ramp up, into B's north face

    # band 3: FOOD = NOR(FX_L, FY_L) -> food lamp
    gate(24, 27); lane_row(6, 27, 11)
    upper(14, 27)
    b.lamp(X(15), MID, Z(27)); b.wire(X(15), UP, Z(27))   # food lamp under its wire

    # ── RS latch at z=28: A (18), torch east (19) -> (20),(21) -> B (22), torch east (23)
    b.glow(X(18), MID, Z(28)); b.solid(X(18), UP, Z(28))
    b.glow(X(19), MID, Z(28)); b.wtorch(X(19), UP, Z(28), 'east')
    upper(20, 28); upper(21, 28)
    b.glow(X(22), MID, Z(28)); b.solid(X(22), UP, Z(28))
    b.glow(X(23), MID, Z(28)); b.wtorch(X(23), UP, Z(28), 'east')
    upper(24, 28)
    # loop: (24,29),(24,30), west along 30 to (18,30), north (18,29) into A's south face
    upper(24, 29); upper(24, 30)
    for x in range(18, 24): upper(x, 30)
    b.w.set((X(21), MID, Z(30)), None); b.lamp(X(21), MID, Z(30))   # body lamp under the loop
    upper(18, 29)
    # Q_L: second torch on A's west face -> (16,28) -> (16,29),(16,30) -> west along 30 -> lane 9
    b.glow(X(17), MID, Z(28)); b.wtorch(X(17), UP, Z(28), 'west')
    upper(16, 28); upper(16, 29); upper(16, 30)
    for x in range(12, 16): upper(x, 30)
    upper_on_solid(11, 30); mid_on_solid(10, 30)           # east-arrival ramp down onto lane 9
    run_wire(b, [(X(9), Z(30)), (X(9), Z(13))], rep_every=4)   # lane 9 flows NORTH
    return {'body_lamp': (X(21), MID, Z(30)), 'food_lamp': (X(15), MID, Z(27)),
            'Q': (X(24), UP, Z(28))}


def through_lanes(b, cx, cz0, cz1):
    """The three select lanes and the DEAD collector for one column, spanning
    rows of cells from cz0 to cz1 (exclusive of the last cell's end)."""
    for lx in (0, 3, 6):
        avoid = {(cx + lx, cz + z) for cz in range(cz0, cz1, W) for z in (3, 11, 19, 27)}
        run_wire(b, [(cx + lx, cz0 - 1), (cx + lx, cz1)], rep_every=8, avoid=avoid)
    avoid = {(cx + 27, cz + 11) for cz in range(cz0, cz1, W)}
    run_wire(b, [(cx + 27, cz0), (cx + 27, cz1 + 1)], rep_every=8, avoid=avoid)


def through_rows(p, cx0, cx1, cz):
    """The four global rows of one cell-row across columns cx0..cx1."""
    for rz in (0, 8, 16, 24):
        p.row(cx0 - 1, cx1, cz + rz, rep_every=8,
              avoid={(cx + 12, cz + rz) for cx in range(cx0, cx1, W)})
