"""Lock-latch register bit driven by fabric lanes.

    reg_bit(f, name, d_lane, write_lane, x, z)

Storage repeater R at (x+1, z) reading the D cell (x, z); Q wire at (x+2, z),
(x+3, z). Lock chain SOUTH of R: lock repeater (x+1, z+1) facing south, torch
NOT WRITE at (x+1, z+2) facing north on block (x+1, z+3), WRITE stub = the
last cell of a lane flowing NORTH that ends at (x+1, z+4).
  d_lane      slot x,   flows south, ends at (x, z)
  write_lane  slot x+1, flows north, ends at (x+1, z+4)   (its gate sits
              south of the register, so the two lanes never share a z)
Q is exposed as lane `name` at slot x+3 flowing south from z+1.
The write pulse is a 4-tick pulse; R is unlocked from +4 to +8 after it rises.
"""
from build import Y


def reg_bit(f, name, d_lane, write_lane, x, z, lamp=False):
    b = f.b
    assert d_lane.x == x and d_lane.flow == 'south', (name, d_lane.x, x)
    assert write_lane.x == x + 1 and write_lane.flow == 'north', (name, write_lane.x, write_lane.flow)
    d_lane.extend(z)
    write_lane.extend(z + 4)
    b.repeater(x + 1, Y, z, 'west')
    b.wire(x + 2, Y, z); b.wire(x + 3, Y, z)
    b.repeater(x + 1, Y, z + 1, 'south')
    b.wtorch(x + 1, Y, z + 2, 'north')
    b.solid(x + 1, Y, z + 3)
    b.wire(x + 3, Y, z + 1)
    if lamp:
        b.lamp(x + 3, Y - 1, z + 1)
    return f.new_lane(name, x + 3, z + 1, 'south', loose=True)
