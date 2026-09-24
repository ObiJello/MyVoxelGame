"""Select matrices around the Snake board: dust-OR decoders made of
repeater taps, no gates.

A board column HX_L[i] must be LOW exactly when the head's X register
equals i, i.e. it is the OR of one line per bit: Q_b when bit b of i is 0,
NQ_b when it is 1. Every such line is tapped into the column by a repeater
(which isolates the line from the column net), so the column is the dust
OR of its taps. Rows work the same way, plus one tap from the phase pulse
line (PSET_L, PCHECK_L or PERASE_L), which makes the row NOR(select,
pulse) with no gate at all.

X side (north of the board): lines run EAST at level 4 on solids (over
everything else), 4 apart in z; columns run SOUTH at level 0. A tap from
line z into column x: repeater (x-5, 4, z+1) reading the line cell north
of it, dust (x-5, 4, z+2), then a four step descent to (x-1, 0, z+2)
beside the column cell. Line repeaters sit at x = 12 (mod 16); taps use
x = 11..15, 0..4 and 5..9 mod 16 in the descent row z+2 only, and the
repeater/first cell at x = 11, 0, 5 (mod 16) in rows z+1/z+2, so a line
repeater at 12 (mod 16) is never a tap's read cell.

Y side (west of the board): lines run SOUTH at level 0, 6 apart in x
(fabric slots); rows run EAST at level 2. A tap from line x into row z on
side s (+1 south of the row or -1 north): repeater (x+1, 0, z+s) reading
the line cell west of it, dust (x+2, 0, z+s), dust (x+3, 1, z+s) on a
solid, dust (x+4, 2, z+s) on a solid, beside the row cell (x+4, 2, z).
"""
from pixel import L0, L1, L2, W as PW

XL, XSOL = 2, 1              # x lines at level 2 on glowstone at level 1


XLV = 4                      # the X select lines run on level 4 (over the fabric's rows)


def xline(b, z, x_from, x_to, rep_mod=12):
    """East-flowing line at level XLV on glowstone from x_from to x_to
    (inclusive); a repeater wherever x % 16 == rep_mod (even on the first
    cell: a repeater may follow a repeater). Glowstone, not solid: a fabric
    torch under a solid would power it and leak into the line."""
    for x in range(x_from, x_to + 1):
        if b.w.blocks.get((x, XLV - 1, z)) is None:
            b.glow(x, XLV - 1, z)
        if x % PW == rep_mod:
            b.repeater(x, XLV, z, 'west')
        else:
            b.wire(x, XLV, z)


def xvia_up(b, x, z):
    """Climb from the level-0 lane cell (x, 0, z) east to level XLV: two
    dust steps, a repeater on level 2 (a lane's last cell may be as weak
    as 3) whose front block carries the next step, two more steps, then a
    repeater at x+6; the line starts at x+7. Keep the fabric off x-1..x+7
    at this z. Glowstone sits over the neighbouring slot at x+3."""
    b.solid(x + 1, 0, z); b.wire(x + 1, 1, z)
    b.solid(x + 2, 1, z); b.wire(x + 2, 2, z)
    b.glow(x + 3, 1, z); b.repeater(x + 3, 2, z, 'west')
    b.solid(x + 4, 2, z); b.wire(x + 4, 3, z)          # dust on the repeater's strongly powered block
    b.solid(x + 5, 3, z); b.wire(x + 5, 4, z)
    b.glow(x + 6, 3, z); b.repeater(x + 6, XLV, z, 'west')
    return x + 7


def xtap(b, x, z):
    """Tap the level-XLV line at z into the level-0 column at x: a repeater
    at (x-5, 4, z+1) reading the line cell north of it, dust (x-5, 4, z+2),
    then a descent one level per cell to (x-1, 0, z+2) beside the column."""
    xr = x - XLV - 1
    b.glow(xr, XLV - 1, z + 1); b.repeater(xr, XLV, z + 1, 'north')
    b.solid(xr, XLV - 1, z + 2); b.wire(xr, XLV, z + 2)     # solid: dust below reads a wire above only through a conductor
    for i in range(1, XLV + 1):
        lv = XLV - i
        if lv > 0:
            b.solid(xr + i, lv - 1, z + 2)
        b.wire(xr + i, lv, z + 2)


def _anchored(lo, hi, avoid, every=12):
    """Repeater positions from hi downwards every `every` cells (hi is the
    last cell before the board, whose own first repeater is within 15
    cells), shifted back one cell while they land on an `avoid` cell."""
    out = set()
    p = hi
    while p > lo + 1:
        q = p
        while q in avoid and q > lo + 1:
            q -= 1
        if q > lo + 1:
            out.add(q)
        p = q - every
    return out


def xcolumn(b, x, z_top, z_bottom, join_zs, own_joins=()):
    """South-flowing column at level 0 from z_top to z_bottom (inclusive):
    a repeater right after each of the column's own tap joins (a join
    arrives with strength ~10 after the descent) and otherwise anchored
    from z_bottom - 1 every 12 cells, never on a join cell."""
    reps = _anchored(z_top, z_bottom - 1, set(join_zs) | {j + 1 for j in own_joins})
    reps |= {j + 1 for j in own_joins if j + 1 < z_bottom}
    for z in range(z_top, z_bottom + 1):
        if z in reps:
            b.repeater(x, L0, z, 'north')
        else:
            b.wire(x, L0, z)


def xmatrix(b, board_x0, board_z0, groups, x_last=None, z_bot=None):
    """groups: list of (name, lines, columns): `lines` is a dict
    bitname -> z of the level-2 line (the caller feeds the line's west end
    at (x_line_from, 2, z)); `columns` is a list of (x, [bitnames]) for the
    board columns of this group. Lines span from x_from to the last
    column. Columns run from z_top (the topmost line - 2) to board_z0 - 1.
    Returns nothing; the caller lays the line sources."""
    if x_last is None:
        x_last = board_x0 + PW * 8 - 1
    all_z = [z for _, lines, _ in groups for z in lines.values()]
    z_top = min(all_z) - 2
    if z_bot is None:
        z_bot = board_z0 - 1
    for name, lines, columns in groups:
        for z in lines.values():
            xline(b, z, board_x0 - 6, x_last)
    join_cells = set()
    for name, lines, columns in groups:
        for x, bits in columns:
            for bit in bits:
                xtap(b, x, lines[bit])
                join_cells.add(lines[bit] + 2)
    for name, lines, columns in groups:
        for x, bits in columns:
            xcolumn(b, x, z_top, z_bot, join_cells, own_joins=[lines[bit] + 2 for bit in bits])
    return z_top


def ytap(b, x, z, side, east=True):
    """Tap the level-0 line at x into the level-2 row at z from side
    (+1: the tap sits south of the row, -1: north). The tap climbs east of
    the line (cells x+1..x+4) or, with east=False, west of it (x-1..x-4);
    a line's taps all use one side, so lines 3 apart can alternate."""
    zt = z + side
    d = 1 if east else -1
    b.repeater(x + d, L0, zt, 'west' if east else 'east')
    b.wire(x + 2 * d, L0, zt)
    b.solid(x + 3 * d, L0, zt); b.wire(x + 3 * d, L1, zt)
    b.solid(x + 4 * d, L1, zt); b.wire(x + 4 * d, L2, zt)


def ytap_join(x, east=True):
    """The row cell a tap on line x joins."""
    return x + 4 if east else x - 4


def yline(b, x, z_from, z_to, tap_zs):
    """South-flowing line at level 0 from z_from to z_to (inclusive) with
    repeaters every 12 cells, never on a tap's read cell."""
    reps = _anchored(z_from, z_to - 1, tap_zs)
    for z in range(z_from, z_to + 1):
        if z in reps:
            b.repeater(x, L0, z, 'north')
        else:
            b.wire(x, L0, z)


def yrow(b, z, x_from, x_to, tap_xs):
    """East-flowing row at level 2 on glowstone from x_from to x_to
    (inclusive, x_to = the cell before the board), repeaters anchored at
    x_to - 1 and every 12 cells back, never on a tap's join cell."""
    reps = _anchored(x_from, x_to - 1, tap_xs)
    for x in range(x_from, x_to + 1):
        if b.w.blocks.get((x, L1, z)) is None:
            b.glow(x, L1, z)
        if x in reps:
            b.repeater(x, L2, z, 'west')
        else:
            b.wire(x, L2, z)


# which side of each board row the taps sit on (see pixel.ROW_Z: rows at
# 0, 8, 10, 14 within the 16-cell pitch)
ROW_SIDE = {0: +1, 8: -1, 10: +1, 14: -1}


def ymatrix(b, board_x0, board_z0, lines, rows, z_from):
    """lines: dict name -> x of a south-flowing level-0 line (fed by the
    caller at (x, 0, z_from - 1)); rows: list of (z, [line names]) for the
    board rows (z absolute). Lines run from z_from to the last tap. Rows
    run from the westmost tap join to board_x0 - 1."""
    tap_z = {n: set() for n in lines}
    for z, names in rows:
        side = ROW_SIDE[(z - board_z0) % PW]
        for n in names:
            tap_z[n].add(z + side)
    for n, x in lines.items():
        if tap_z[n]:
            yline(b, x, z_from, max(tap_z[n]) + 1, tap_z[n])
    for z, names in rows:
        side = ROW_SIDE[(z - board_z0) % PW]
        joins = set()
        for n in names:
            ytap(b, lines[n], z, side)
            joins.add(lines[n] + 4)
        yrow(b, z, min(lines[n] for n in names) + 4, board_x0 - 1, joins)
