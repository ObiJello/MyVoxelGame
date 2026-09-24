"""The Snake board as display blocks (redstone_plus).

panel(b, x0, z0) lays 8x8 pixels at pitch PITCH: pixel (i, j)'s root is
(x0 + 12 i + 6, 0, z0 + 12 j + 2) and its run is the 8 display blocks
y = 0..7 (see RedstoneComponents.cpp "DisplayBlock" / redsim.display_eval):

    y 0  op block: north&west = set_g (HYS & HX), south&east = clear_g (TYE & TX)
    y 2  its column faces (the op block reads the block two above)
    y 4  op block: north&west = check_g (HYC & HX2), south&east = show_r (FY & FX)
    y 6  its column faces
    y 8  op block: north&west = clear_g (CLR row & CLR column): the restart's clear
    y 10 its column faces
    y 11..19 plain run, clear of every line; dust on top of it must not sit
         beside the level-10 columns
    y 20 dust on top: the check output, one COL line per pixel column

The wall (the display the player looks at) is a FRAME BUFFER: each 3x3
face at x = WALL_X is its own display group with a `latch` op block in a
stub behind it (x = WALL_X-2, three tall). A chain of display blocks (one
group with the pillar, so it carries the pixel's whole colour) climbs
from (px+1, 19) to level L(i) = WALL_L0 + 2*(7-i), runs east along the
pixel row's z, south along x = XT(j) = WALL_XT0 - 2*j to z_i - 1 (the
wall column z_i = WALL_Z0 + 4*(7-i)), down to the wall row y_j = WALL_Y0
+ 4*(7-j) and east to the cell north of the stub: the latch's data face.
The latch's clock is the stub's top block's west face, touched by a
PRESENT dust line (level y_j+2, x = WALL_X-3) per wall row; the eight
lines hang off one staircase fed at the 'PRESENT' port. While PRESENT is
powered every face copies its pixel; the machine strobes it once per
cycle after the board is fully updated, so the wall only ever shows
complete frames. Levels per board column and lanes per board row keep
the 64 chains apart (_wall_chain). The viewer stands east of the wall
looking west: board column 0 is on the left (south), board row 0 at the
top; the deck's D-pad's "far" is up and its right-hand button (E) moves
right.

Lines inside the panel, all active-HIGH (the ports invert the matrix's
active-low select lines with a blue torch and shift them to their level):
    rows (along x, glowstone below): HYS y0 z=pz-1, TYE y0 z=pz+1,
                                     HYC y4 z=pz-1, FY  y4 z=pz+1
    columns (along z):               HX  y2 x=px-1, TX  y2 x=px+1,
                                     HX2 y6 x=px-1 (a copy of HX), FX y6 x=px+1
    CLEAR: one net: rows at y8 z=pz-1 joined at x0-1, columns at y10
           x=px-1 joined at z0-3 — powered = every pixel cleared
Rows and columns cross two levels apart, so they never touch; every dust
line sits on glowstone (a solid under a powered dust is strongly powered
and would drive any dust beside it). (Dust under the roots, the block's
own clear input, is out: a level-0 row cell beside a root reads it
diagonally through the root, which is not a conductor.)

Matrix side (what the caller lays): rows arrive flowing EAST at level 2
ending at (x0 - PORT_W - 1, 2, z) with z = z0 + 12 j + ROW_Z[name], taps
on the +z side; columns arrive flowing SOUTH at level 0 ending at
(x0 + 12 i + COL_X[name], 0, z0 - PORT_N - 1).
Returns the ports: 'HX'/'TX'/'FX'[i] and 'HYS'/'HYC'/'TYE'/'FY'[j] = those
end cells, 'COL' = the merged check output cell (dust, level 12, south of the
panel, west end), 'CLEAR' = the clear net's entry cell (dust, level 8, the
south end of the row-join line at x0-1), 'pixel'[(i, j)] = the root,
'wall'[(i, j)] = the centre of the pixel's wall face, 'PRESENT' = the
frame-buffer strobe's entry cell (dust, level 0, south of the wall),
'x_last' = the panel's last column-line x.
"""
from build import Y

N = 8
PITCH = 12
ROW_Z = {'HYS': 1, 'HYC': 4, 'TYE': 7, 'FY': 10}     # matrix rows within a pitch (taps all on side +1)
COL_X = {'HX': 3, 'TX': 7, 'FX': 11}                 # matrix columns within a pitch
PORT_W = 13                                          # row ports: x0-PORT_W .. x0-1
PORT_N = 15                                          # column ports: z0-PORT_N .. z0-1
SIZE = N * PITCH                                     # 96
OPS = {0: ('set_g', 'clear_g'), 4: ('check_g', 'show_r'), 8: ('clear_g', 'none')}
RUN = 20                                             # pillar height; the lines stop at level 10
WALL_L0 = 40                                         # chain levels 40..54 (per board column, above everything)
WALL_XT0 = 107                                       # chain lanes x 93..107 (per board row), east of the last pillar's climb
WALL_Z0 = 112                                        # wall columns z 112..140 (south of the panel and its COL merge line)
WALL_Y0 = 4                                          # wall rows y 4..32
WALL_X = 112                                         # the wall's x (one block thick, faces east)
WALL_FACE = 1                                        # half-size of a pixel's face: 3x3
WALL_FILL = 'minecraft:black_concrete'               # a backing plane behind the wall (seen through the gaps) and its frame
WALL_STUB = WALL_X - 2                               # the latch stubs' x; the chains end at (WALL_STUB, y_j, z_i - 1)
PRESENT_X = WALL_X - 3                               # the PRESENT lines' x (level y_j + 2, touching the stub tops' west faces)


def _line_x(b, y, z, x_from, x_to):
    for x in range(x_from, x_to + 1):
        b.glow(x, y - 1, z); b.wire(x, y, z)


def _line_z(b, y, x, z_from, z_to):
    for z in range(z_from, z_to + 1):
        b.glow(x, y - 1, z); b.wire(x, y, z)


def _row_port(b, xa, zr, yt, zo, xj):
    """A matrix row ending at (xa, 2, zr): shift to level yt (0 or 4),
    invert, jog to zo along x = xj, and run east to xa + PORT_W."""
    if yt == 0:
        b.solid(xa + 1, 1, zr); b.wire(xa + 1, 2, zr)        # solid: the next cell reads a wire above only through a conductor
        b.solid(xa + 2, 0, zr); b.wire(xa + 2, 1, zr)
        b.glow(xa + 3, -1, zr); b.wire(xa + 3, 0, zr)
        b.solid(xa + 4, 0, zr); b.wtorch(xa + 5, 0, zr, 'east')
        x = xa + 6
    else:
        b.solid(xa + 1, 2, zr); b.wire(xa + 1, 3, zr)
        b.solid(xa + 2, 3, zr); b.wire(xa + 2, 4, zr)
        b.solid(xa + 3, 4, zr); b.wtorch(xa + 4, 4, zr, 'east')
        x = xa + 5
    x_end = xa + PORT_W
    if zo == zr:
        _line_x(b, yt, zr, x, x_end)
        return
    _line_x(b, yt, zr, x, xj)
    step = 1 if zo > zr else -1
    for z in range(zr + step, zo + step, step):
        b.glow(xj, yt - 1, z); b.wire(xj, yt, z)
    _line_x(b, yt, zo, xj + 1, x_end)


def _rise(b, x, za, yt):
    """Climb from the level-0 cell (x, 0, za) south to level yt: dust at
    (x, k, za + k) on a solid, k = 1..yt. Returns the last cell's z."""
    for k in range(1, yt + 1):
        b.solid(x, k - 1, za + k); b.wire(x, k, za + k)
    return za + yt


def _col_port(b, xc, za, yt, xo, zj, z_end, fan6=None):
    """A matrix column ending at (xc, 0, za): rise to yt, invert (torch
    facing south), jog to x = xo along z = zj and run south to z_end.
    fan6: with (zj6,) also branch a copy up to level 6, jogging at zj6."""
    z = _rise(b, xc, za, yt)
    b.solid(xc, yt, z + 1); b.wtorch(xc, yt, z + 2, 'south')
    z = z + 3
    _line_z(b, yt, xc, z, zj)
    if xo != xc:
        step = 1 if xo > xc else -1
        for x in range(xc + step, xo + step, step):
            b.glow(x, yt - 1, zj); b.wire(x, yt, zj)
    _line_z(b, yt, xo, zj + 1, z_end)
    if fan6 is not None:
        (zj6,) = fan6
        # from the level-2 line cell at (xc, yt, zj): a solid south of it carries the first step up
        zz = zj
        for k in range(yt + 1, 7):
            zz += 1
            b.solid(xc, k - 1, zz); b.wire(xc, k, zz)
        b.glow(xc, 5, zz + 1); b.wire(xc, 6, zz + 1)
        assert zz + 1 <= zj6, (zz, zj6)
        _line_z(b, 6, xc, zz + 2, zj6)
        step = 1 if xo > xc else -1
        for x in range(xc + step, xo + step, step):
            b.glow(x, 5, zj6); b.wire(x, 6, zj6)
        _line_z(b, 6, xo, zj6 + 1, z_end)


def _wall_chain(b, i, j, px, pz):
    """The display-block chain from pillar (i, j) to the latch stub of its
    wall face, the stub, the link and the face; returns the face's centre."""
    L = WALL_L0 + 2 * (N - 1 - i)
    xt = WALL_XT0 - 2 * j
    zw = WALL_Z0 + 4 * (N - 1 - i)
    yw = WALL_Y0 + 4 * (N - 1 - j)
    assert xt > px + 2 and zw - 1 > pz and L > yw + 1 and xt < WALL_STUB, (i, j, px, pz, xt, zw, L, yw)
    b.display(px + 1, RUN - 1, pz)                                   # side step off the pillar (its top keeps the read-back dust)
    for y in range(RUN, L + 1):
        b.display(px + 1, y, pz)                                     # climb
    for x in range(px + 2, xt + 1):
        b.display(x, L, pz)                                          # east
    for z in range(pz + 1, zw):
        b.display(xt, L, z)                                          # south, to the lane north of the wall column
    for y in range(yw, L):
        b.display(xt, y, zw - 1)                                     # down
    for x in range(xt + 1, WALL_STUB + 1):
        b.display(x, yw, zw - 1)                                     # east: the last cell is the latch's data source
    # the frame-buffer pixel: latch stub (its top block's west face is the clock), link, face
    b.display(WALL_STUB, yw, zw, 'latch', 'none')
    b.display(WALL_STUB, yw + 1, zw); b.display(WALL_STUB, yw + 2, zw)
    b.display(WALL_STUB + 1, yw, zw)
    for dy in range(-WALL_FACE, WALL_FACE + 1):
        for dz in range(-WALL_FACE, WALL_FACE + 1):
            b.display(WALL_X, yw + dy, zw + dz)
    return (WALL_X, yw, zw)


def panel(b, x0, z0):
    xa = x0 - PORT_W - 1                  # the matrix rows' last cell
    za = z0 - PORT_N - 1                  # the matrix columns' last cell
    x_end = x0 + SIZE - 1
    z_end = z0 + SIZE - 1
    pixel = {}
    wall = {}
    for j in range(N):
        pz = z0 + PITCH * j + 2
        zr = z0 + PITCH * j
        # pixels
        for i in range(N):
            px = x0 + PITCH * i + 6
            for y in range(RUN):
                nw, se = OPS.get(y, ('none', 'none'))
                b.display(px, y, pz, nw, se)
            pixel[(i, j)] = (px, 0, pz)
            wall[(i, j)] = _wall_chain(b, i, j, px, pz)
        # rows: port (level shift, inverter, jog) then the panel line
        _row_port(b, xa, zr + ROW_Z['HYS'], 0, pz - 1, None)
        _row_port(b, xa, zr + ROW_Z['TYE'], 0, pz + 1, xa + 8)
        _row_port(b, xa, zr + ROW_Z['HYC'], 4, pz - 1, xa + 7)
        _row_port(b, xa, zr + ROW_Z['FY'], 4, pz + 1, xa + 9)
        for y, z in ((0, pz - 1), (0, pz + 1), (4, pz - 1), (4, pz + 1), (8, pz - 1)):
            _line_x(b, y, z, x0, x_end)
    # a dark backing plane one block behind the wall, with holes where the chains enter the faces:
    # the gaps between faces show it, and it hides the chains. Never IN the wall plane: a solid on
    # top of a face is strongly powered by the pixel's check output and reads as a clear into the
    # face above it (a display block's bottom face).
    y_lo, y_hi = WALL_Y0 - WALL_FACE - 1, WALL_Y0 + 4 * (N - 1) + WALL_FACE + 1
    z_lo, z_hi = WALL_Z0 - WALL_FACE - 1, WALL_Z0 + 4 * (N - 1) + WALL_FACE + 1
    for y in range(y_lo, y_hi + 1):
        for z in range(z_lo, z_hi + 1):
            if b.w.blocks.get((WALL_X - 1, y, z)) is None:
                b.solid(WALL_X - 1, y, z, WALL_FILL)
    # PRESENT: a staircase north along x = PRESENT_X - 1 from the port at level 0, z = z_hi + 3, and at every
    # wall row's clock level (y_j + 2) a line at x = PRESENT_X along the wall (glowstone everywhere)
    zp = z_hi + 3
    present = (PRESENT_X - 1, 0, zp)
    clocks = {WALL_Y0 + 4 * (N - 1 - j) + 2 for j in range(N)}
    for k in range(0, max(clocks) + 1):
        b.glow(PRESENT_X - 1, k - 1, zp - k); b.wire(PRESENT_X - 1, k, zp - k)
        if k in clocks:                                               # the staircase cell (z = zp - k) lies beside the line
            assert z_lo <= zp - k <= z_hi, (k, zp - k)
            for z in range(z_lo, z_hi + 1):
                b.glow(PRESENT_X, k - 1, z); b.wire(PRESENT_X, k, z)
    # columns
    for i in range(N):
        px = x0 + PITCH * i + 6
        xc = x0 + PITCH * i
        _col_port(b, xc + COL_X['HX'], za, 2, px - 1, za + 6, z_end, fan6=(za + 11,))
        _col_port(b, xc + COL_X['TX'], za, 2, px + 1, za + 5, z_end)
        _col_port(b, xc + COL_X['FX'], za, 6, px + 1, za + 9, z_end)
        # the CLEAR column (level 10) from the column-join line (z0 - 3) south
        _line_z(b, 10, px - 1, z0 - 2, z_end)
        # the check output: dust on top of the run, one line per pixel column, merged south of the panel
        for z in range(z0, z_end + 3):
            if (z - z0) % PITCH != 2:
                b.glow(px, RUN - 1, z)
            b.wire(px, RUN, z)
    zc = z_end + 2
    for x in range(x0 + 6, x0 + PITCH * (N - 1) + 6 + 1):
        if b.w.blocks.get((x, RUN - 1, zc)) is None:
            b.glow(x, RUN - 1, zc)
        if b.w.blocks.get((x, RUN, zc)) is None:
            b.wire(x, RUN, zc)
    # the CLEAR net: the row-join line (x0-1, level 8) from the entry south of the panel north to
    # z0 - 1, a two-step rise, and the column-join line (level 10) along z0 - 3
    xj = x0 - 1
    _line_z(b, 8, xj, z0 - 1, z_end + 3)
    b.solid(xj, 8, z0 - 2); b.wire(xj, 9, z0 - 2)
    b.solid(xj, 9, z0 - 3); b.wire(xj, 10, z0 - 3)
    _line_x(b, 10, z0 - 3, x0, x0 + PITCH * (N - 1) + 5)
    return {
        'HX': [(x0 + PITCH * i + COL_X['HX'], 0, za) for i in range(N)],
        'TX': [(x0 + PITCH * i + COL_X['TX'], 0, za) for i in range(N)],
        'FX': [(x0 + PITCH * i + COL_X['FX'], 0, za) for i in range(N)],
        'HYS': [(xa, 2, z0 + PITCH * j + ROW_Z['HYS']) for j in range(N)],
        'HYC': [(xa, 2, z0 + PITCH * j + ROW_Z['HYC']) for j in range(N)],
        'TYE': [(xa, 2, z0 + PITCH * j + ROW_Z['TYE']) for j in range(N)],
        'FY': [(xa, 2, z0 + PITCH * j + ROW_Z['FY']) for j in range(N)],
        'COL': (x0 + 6, RUN, zc),
        'CLEAR': (xj, 8, z_end + 3),
        'pixel': pixel,
        'wall': wall,
        'PRESENT': present,
        'x_last': x_end,
    }
