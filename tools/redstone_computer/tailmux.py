"""Tail-direction mux as a tap matrix (no gates on the fabric).

The direction FIFO stores the NOT of each direction bit, so a stage's Q
lane is Q_L (low when the stored bit is 1). The length counter L gives
eight lines Q0, NQ0, .., Q3, NQ3. For each stage s (length L = s + 1):

  SEL_L[s] row (level 2, east-west): dust OR of repeater taps from the
      four "must be low" length lines (Q_b when bit b of s+1 is 0, else
      NQ_b), so the row is LOW exactly when L == s + 1.
  block B[s] (level 2, east of the stage's Q_L lane): repeater taps from
      the SEL row (from the north) and from the Q_L lane (a two step climb
      and a repeater from the west); a torch on top is lit iff L == s + 1
      and the stored bit is 1, and powers a block on level 4.
  from that block the outputs run south on level-4 columns to a level-4
      bus that is the dust OR of all stages; the bus descends four steps
      onto the TD lane (level 0, flowing south). Level 4 is empty here,
      so the columns cross the level-2 rows freely.

Rows are laid in reverse order (stage 11 gets the northmost row) and each
row ends at its own stage. Stage pitch is 6 (fifo.py); the climb uses
x+1..x+3, the block, torch and column x+4.

tailmux(b, l_lines, fifo_q, z_top, td_x) lays everything except the
level-0 lines themselves. l_lines: {'Q0': x, 'NQ0': x, ...}; fifo_q: two
lists (bit 0, bit 1) of the 12 stages' Q_L lane x's; td_x: the two TD
lane x's (east of the last stage of each bit, >= last + 9). Returns the z
every line must reach (z_line_end) and the bus z. The caller extends its
lanes to z_line_end and marks the tap cells (so they stay plain dust).
"""
from pixel import L0, L1, L2
from matrix import ytap

ROW_PITCH = 4
STAGES = 12


def bits4(v):
    return ['NQ%d' % b if (v >> b) & 1 else 'Q%d' % b for b in range(4)]


def row_z(z_top, s):
    return z_top + ROW_PITCH * (STAGES - 1 - s)


def tap_cells(l_lines, fifo_q, z_top):
    """z of every line cell read by a tap (per line x)."""
    taps = {}
    for s in range(STAGES):
        z = row_z(z_top, s)
        for n in bits4(s + 1):
            taps.setdefault(l_lines[n], set()).add(z + 1)
        for qs in fifo_q:
            taps.setdefault(qs[s], set()).add(z + 2)
    return taps


def _lay_row(b, z, x_from, x_to, avoid):
    since = 0
    for x in range(x_from, x_to + 1):
        if b.w.blocks.get((x, L1, z)) is None:
            b.glow(x, L1, z)
        since += 1
        if since >= 12 and x not in avoid and x != x_from and x_to - x > 1:
            b.repeater(x, L2, z, 'west'); since = 0
        else:
            b.wire(x, L2, z)


def tailmux(b, l_lines, fifo_q, z_top, td_x):
    """See the module docstring. Output columns and the buses are on
    level 4 (torch on top of each block, then a powered block), so they
    cross the level-2 rows freely; each bus descends four steps east onto
    its TD lane at td_x (the bus ends at td_x - 4)."""
    z_bus = row_z(z_top, 0) + 6
    for s in range(STAGES):
        z = row_z(z_top, s)
        joins = set()
        for n in bits4(s + 1):
            x = l_lines[n]
            ytap(b, x, z, +1)                        # cells x+1..x+4 at z+1, joins the row at x+4
            joins.add(x + 4)
        for qs in fifo_q:
            joins.add(qs[s] + 4)                     # the block's SEL tap reads this row cell
        _lay_row(b, z, min(joins), max(qs[s] for qs in fifo_q) + 4, joins)
        for qs in fifo_q:
            xq = qs[s]
            # Q_L climb at z+2 and a repeater into the block from the west
            b.solid(xq + 1, L0, z + 2); b.wire(xq + 1, L1, z + 2)
            b.solid(xq + 2, L1, z + 2); b.wire(xq + 2, L2, z + 2)
            b.repeater(xq + 3, L2, z + 2, 'west')
            b.solid(xq + 4, L2, z + 2)                       # B[s] = NOR(SEL_L, Q_L)
            b.repeater(xq + 4, L2, z + 1, 'north')           # SEL row tap (reads the row cell north of it)
            b.torch(xq + 4, 3, z + 2)                        # lit iff L == s+1 and the stored bit is 1
            b.solid(xq + 4, 4, z + 2)                        # strongly powered by the torch
            # output column on level 4, south to the bus; a repeater right
            # before the bus so every join injects full strength
            since = 0
            for zz in range(z + 3, z_bus + 1):
                b.solid(xq + 4, 3, zz)
                since += 1
                if (since >= 12 and zz != z + 3 and z_bus - zz > 2) or (zz == z_bus - 1 and zz != z + 3):
                    b.repeater(xq + 4, 4, zz, 'north'); since = 0
                else:
                    b.wire(xq + 4, 4, zz)
    # buses on level 4, flowing east, then four steps down onto the TD lane
    for bit, qs in enumerate(fifo_q):
        x0 = qs[0] + 4
        xe = td_x[bit] - 4
        assert xe >= qs[STAGES - 1] + 5, ('TD lane too close', td_x[bit], qs[STAGES - 1])
        joins = {xq + 4 for xq in qs}
        since = 0
        for x in range(x0, xe + 1):
            if b.w.blocks.get((x, 3, z_bus)) is None:
                b.solid(x, 3, z_bus)
            since += 1
            if since >= 12 and x not in joins and x != x0 and xe - x > 1:
                b.repeater(x, 4, z_bus, 'west'); since = 0
            else:
                b.wire(x, 4, z_bus)
        b.solid(xe + 1, 2, z_bus); b.wire(xe + 1, 3, z_bus)
        b.solid(xe + 2, 1, z_bus); b.wire(xe + 2, 2, z_bus)
        b.solid(xe + 3, 0, z_bus); b.wire(xe + 3, 1, z_bus)
        b.wire(td_x[bit], 0, z_bus)                         # the TD lane's first cell
    return {'z_line_end': row_z(z_top, 0) + 3, 'z_bus': z_bus}
