"""Small combinational cells built from one torch each, in ACTIVE-LOW style.

A solid block with a wall torch is NOR of everything powering the block, so
with active-low inputs (a line that is HIGH when the condition is false) the
torch output is the AND of the conditions. Every input must arrive pointing
straight into the block (the wire's last two cells in line with it) or as a
wire lying on top of the block.

    and_block(b, x, z, out)  -> block at (x, z), torch on face `out`; input
                                faces are the other three sides plus the top.
"""
from build import Builder, Y
from redsim import add, OPP, DIRS


def and_block(b, x, z, out):
    """Returns the torch position (x', z') and a dict of free input cells:
    {'north': (x, z-1), ...} for the three side faces, plus 'top' = (x, z)
    at level Y+1."""
    b.solid(x, Y, z)
    tx, ty, tz = add((x, Y, z), out)
    b.wtorch(tx, ty, tz, out)
    faces = {}
    for d in ('north', 'south', 'east', 'west'):
        if d == out:
            continue
        p = add((x, Y, z), d)
        faces[d] = (p[0], p[2])
    faces['top'] = (x, z)
    return (tx, tz), faces


def feed(b, cell, frm, length=2):
    """Lay a straight wire of `length` cells ending at `cell`, arriving from
    direction `frm` (so it points into the block on the far side). Returns the
    far end (x, z) where the caller's route should connect."""
    x, z = cell
    dx, dz = DIRS[frm][0], DIRS[frm][2]
    pts = [(x + dx * i, z + dz * i) for i in range(length)]
    for (px, pz) in pts:
        b.wire(px, Y, pz)
    return pts[-1]
