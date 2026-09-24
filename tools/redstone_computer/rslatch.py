"""Floor RS latch macro driven by fabric lanes.

    rs_latch(f, name, set_lane, reset_lane, xa, z)

Block A at (xa, z) with torch TA on its east face, block B at (xa+4, z) with
torch TB east. TA's output points into B, TB's output loops round into A's
south face. SET = a lane flowing south that ENDS at (xa, z-1) pointing into
A's north face (so set_lane must be at slot xa); RESET likewise at (xa+4).
Q  = TB's net, exposed as lane `name` at slot xa+7 flowing south from z+1.
Q_L = TA's net through a second torch on A's west face, lane `name`+'_L'
at slot xa-3 flowing NORTH from z.
"""
from build import Y
from cells import run_wire


def rs_latch(f, name, set_lane, reset_lane, xa, z, ql=True):
    """xa must be a multiple of 3: SET at xa, RESET at xa+6, Q at xa+9 (and
    Q_L at xa-3) all land on the slot grid."""
    b = f.b
    if set_lane is not None:
        assert set_lane.x == xa and set_lane.flow == 'south', (name, set_lane.x, xa)
        set_lane.extend(z - 1)
    if reset_lane is not None:
        assert reset_lane.x == xa + 6 and reset_lane.flow == 'south'
        reset_lane.extend(z - 1)
    b.solid(xa, Y, z); b.wtorch(xa + 1, Y, z, 'east')
    for x in range(xa + 2, xa + 6):
        b.wire(x, Y, z)
    b.solid(xa + 6, Y, z); b.wtorch(xa + 7, Y, z, 'east')
    b.wire(xa + 8, Y, z)
    # loop: (xa+8, z+1), (xa+8, z+2), west along z+2 to xa, north into A's south face
    b.wire(xa + 8, Y, z + 1); b.wire(xa + 8, Y, z + 2)
    for x in range(xa, xa + 8):
        b.wire(x, Y, z + 2)
    b.wire(xa, Y, z + 1)
    # Q lane east of the loop corner
    b.wire(xa + 9, Y, z + 1)
    q = f.new_lane(name, xa + 9, z + 1, 'south', loose=True)
    if not ql:
        return q, None
    b.wtorch(xa - 1, Y, z, 'west')
    b.wire(xa - 2, Y, z); b.wire(xa - 3, Y, z)
    qll = f.new_lane(name + '_L', xa - 3, z, 'north', loose=True)
    return q, qll
