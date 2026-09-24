"""Circuit cells for the adder, built with build.Builder and verified in redsim.

Conventions: x east, z south. Every cell is placed in a coordinate frame where
signal COLUMNS run along z (north->south) and the cell works between two
columns 10 apart.
"""
import build
from build import Builder, Y
from redsim import add, OPP

COL_GAP = 10   # distance between the two input columns of a two-input cell


def run_wire(b, pts, y=Y, rep_every=6, avoid=()):
    """Wire along a polyline with repeaters inserted on long straight runs.

    A repeater faces its INPUT, so it faces against the flow of the run. `avoid`
    lists (x, z) cells that must stay plain wire — the cells other circuitry
    taps sideways, since a repeater only passes signal straight through.
    """
    cells = []
    for (x0, z0), (x1, z1) in zip(pts, pts[1:]):
        dx = (x1 > x0) - (x1 < x0)
        dz = (z1 > z0) - (z1 < z0)
        x, z = x0, z0
        if not cells:
            cells.append((x, z, None))
        while (x, z) != (x1, z1):
            x += dx; z += dz
            d = ('east' if dx > 0 else 'west') if dx else ('south' if dz > 0 else 'north')
            cells.append((x, z, d))
    since = 0
    for i, (x, z, d) in enumerate(cells):
        nxt = cells[i + 1][2] if i + 1 < len(cells) else None
        straight = d is not None and nxt == d
        if (straight and since >= rep_every and 0 < i < len(cells) - 1
                and (x, z) not in avoid and not build.PLUS):
            b.repeater(x, y, z, OPP[d])   # input from behind (against flow)
            since = 0
        else:
            b.wire(x, y, z)
            since += 1


def xor_cell(b, u, v, z0):
    """A XOR B between column U (x=u) and column V (x=v=u+10), rows z0-2..z0+6.

    Two subtract-mode comparators: C1 rear=U side=V, C2 rear=V side=U, ORed.
    Returns the output cell (x, z) on the OR wire from which the result leaves
    the cell SOUTH over a bridge (call `xor_exit`).
    """
    assert v == u + COL_GAP
    # C1: rear from U
    b.repeater(u + 1, Y, z0, 'west')
    b.comparator(u + 2, Y, z0, 'west', 'subtract')
    b.wire(u + 3, Y, z0)
    # C1 side from V: tap row at z0-2 from x=u+2..v-1, repeater at (u+2, z0-1)
    b.repeater(u + 2, Y, z0 - 1, 'north')
    b.line(u + 2, z0 - 2, v - 1, z0 - 2)
    b.repeater(u + 6, Y, z0 - 2, 'east')      # refresh the tap (flows west)
    # C2: rear from V
    b.repeater(v - 1, Y, z0 + 4, 'east')
    b.comparator(v - 2, Y, z0 + 4, 'east', 'subtract')
    b.wire(v - 3, Y, z0 + 4)
    # C2 side from U: tap row at z0+6 from x=u+1..v-2, repeater at (v-2, z0+5)
    b.repeater(v - 2, Y, z0 + 5, 'south')
    b.line(u + 1, z0 + 6, v - 2, z0 + 6)
    b.repeater(u + 6, Y, z0 + 6, 'west')      # refresh the tap (flows east)
    # OR: C1 out (u+3, z0) -> (u+3, z0+2) -> (v-3, z0+2) -> (v-3, z0+4)
    b.path([(u + 3, z0), (u + 3, z0 + 2), (v - 3, z0 + 2), (v - 3, z0 + 4)])
    return (u + 5, z0 + 2)


def xor_exit_south(b, u, v, z0):
    """Bridge the XOR result south over its own tap row (z0+6) at x=u+5.
    Returns the (x, z) of the level wire south of the bridge."""
    x = u + 5
    b.line(x, z0 + 2, x, z0 + 3)
    b.bridge(x, z0 + 6, 'z', 'south')   # cells z0+3 .. z0+9
    return (x, z0 + 9)


def and_cell(b, u, v, zm):
    """A AND B between columns U and V with torches.

    Taps are L-shaped so their last cell points straight into the input
    block (a wire only powers a block it points at): row zm-3 from U
    (x=u+1..u+4) then a stub south at (u+4, zm-2) into the block (u+4, zm-1);
    row zm+3 from V (x=u+4..v-1) then a stub north at (u+4, zm+2) into the
    block (u+4, zm+1). Torches on top of both blocks light a wire on the
    middle block (u+4, zm); the wall torch on its east face is A AND B, and
    the single output wire east of it at (u+6, zm) is returned. The caller
    continues north or south from there, never east (that is column V) and
    not past row zm+3 (V's tap row).
    """
    xm = u + 4
    b.path([(u + 1, zm - 3), (xm, zm - 3), (xm, zm - 2)])
    b.solid(xm, Y, zm - 1); b.torch(xm, Y + 1, zm - 1)
    b.path([(v - 1, zm + 3), (xm, zm + 3), (xm, zm + 2)])
    b.solid(xm, Y, zm + 1); b.torch(xm, Y + 1, zm + 1)
    b.solid(xm, Y, zm); b.wire(xm, Y + 1, zm)
    b.wtorch(xm + 1, Y, zm, 'east')
    b.wire(xm + 2, Y, zm)
    return (xm + 2, zm)
