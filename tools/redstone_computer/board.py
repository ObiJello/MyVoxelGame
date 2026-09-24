"""The 8x8 Snake board: 64 dense pixels and their shared lines.

board(b, x0, z0) lays pixels (i, j) at (x0 + 16 i, z0 + 16 j). Lines:
  columns (level 0, flowing south, active-low): for pixel column i,
      HX_L at x0+16i, TX_L at x0+16i+5, FX_L at x0+16i+10. They enter at
      z0-1 (the driver's last cell must be at z0-2 or a repeater at z0-1).
  rows (level 2 on glowstone, flowing east, active-low): for pixel row j,
      HYS_L at z0+16j, HYC_L at z0+16j+8, TYE_L at z0+16j+10, FY_L at
      z0+16j+14; they enter at x0-1.
  collision OR lines (level 4, flowing south, active-high): one per pixel
      column at x0+16i+2, leaving the board at z0+128.
Returns a dict of port cells: 'HX'[i], 'TX'[i], 'FX'[i] (first column cell
at z0-1), 'HYS'[j], 'HYC'[j], 'TYE'[j], 'FY'[j] (first row cell at x0-1),
'COL'[i] (last OR cell at z0+128), 'body'[(i,j)], 'food'[(i,j)] lamps.
"""
from pixel import pixel, through_columns, through_rows, W, COL_X, OR_X, ROW_Z, L0, L2, L4

N = 8
SIZE = N * W                      # 128


def board(b, x0, z0):
    body, food = {}, {}
    for j in range(N):
        for i in range(N):
            info = pixel(b, x0 + W * i, z0 + W * j)
            body[(i, j)] = info['body_lamp']; food[(i, j)] = info['food_lamp']
    # lines: laid after the pixels so that pixel-owned cells are kept
    for i in range(N):
        through_columns(b, x0 + W * i, z0, z0 - 1, z0 + SIZE)
    for j in range(N):
        through_rows(b, x0, z0 + W * j, x0 - 1, x0 + SIZE - 1)
    ports = {
        'CLEAR': [(x0 - 1, L4, z0 + W * j + 15) for j in range(N)],
        'HX': [(x0 + W * i + COL_X[0], L0, z0 - 1) for i in range(N)],
        'TX': [(x0 + W * i + COL_X[1], L0, z0 - 1) for i in range(N)],
        'FX': [(x0 + W * i + COL_X[2], L0, z0 - 1) for i in range(N)],
        'HYS': [(x0 - 1, L2, z0 + W * j + ROW_Z[0]) for j in range(N)],
        'HYC': [(x0 - 1, L2, z0 + W * j + ROW_Z[1]) for j in range(N)],
        'TYE': [(x0 - 1, L2, z0 + W * j + ROW_Z[2]) for j in range(N)],
        'FY': [(x0 - 1, L2, z0 + W * j + ROW_Z[3]) for j in range(N)],
        'COL': [(x0 + W * i + OR_X, L4, z0 + SIZE) for i in range(N)],
        'body': body, 'food': food,
    }
    return ports


def clear_lines(b, x0, z0):
    """Restart support (redstone_plus machines): one CLEAR line per pixel row,
    level 4 along z0+16j+15 on glowstone, entering at x0-1 and running east
    across the row. It hops over each pixel's collision OR line (level 4 at
    x0+16i+2) on a two-step glowstone bridge, and in every pixel a stub turns
    north at x0+16i+7 to a repeater at x0+16i+8 that points east into the
    latch's ERASE block B: while CLEAR is powered every pixel is erased,
    exactly as the matrix's erase would do it. Glowstone everywhere so the
    line neither powers nor is powered by the rows below and the loop above.
    """
    L5, L6 = L4 + 1, L4 + 2
    for j in range(N):
        z = z0 + W * j + 15
        for x in range(x0 - 1, x0 + SIZE):
            r = (x - x0) % W
            # the hop's supports are SOLID: dust only climbs onto / down from a conductor
            # (a glowstone step makes the next cell look under it instead — into the OR line)
            if r in (1, 3):
                b.solid(x, L4, z); b.wire(x, L5, z)           # ramp
            elif r == 2:
                b.solid(x, L5, z); b.wire(x, L6, z)           # over the OR line (the solid above its dust also
            else:                                             # cuts the OR line off from the ramp cells)
                b.glow(x, L4 - 1, z); b.wire(x, L4, z)
        for i in range(N):
            px = x0 + W * i
            bz = z0 + W * j + 12                              # the latch's B block row
            for zz in (bz + 2, bz + 1, bz):
                b.glow(px + 7, L4 - 1, zz); b.wire(px + 7, L4, zz)
            b.glow(px + 8, L4 - 1, bz); b.repeater(px + 8, L4, bz, 'west')   # into B at px+9
