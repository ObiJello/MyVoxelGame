"""Direction FIFO: a staircase of lock-latch bits with delay-4 links.

fifo_bit(f, name, prev_q, write_lane, x, z)
    link repeater (delay 4) at (x, z) reading the previous Q lane, which is
    at slot x-3 and is extended down to z with two floor cells east of it;
    storage repeater at (x+1, z); Q at slot x+3 flowing south from z+1;
    write lane (north-flowing) ends at (x+1, z+4).

fifo(f, name, d_lane, stages, x0, z0, write_lanes)
    stage k at (x0 + 6k, z0 + 8k). d_lane must be at slot x0-3.
    write_lanes[k] must be a north-flowing lane at slot x0+6k+1 (created by
    the caller AFTER the fifo, with its gate south of every stage).
    Returns the list of Q lanes.
"""
from build import Y


def fifo_bit(f, name, prev_q, write_lane, x, z):
    b = f.b
    assert prev_q.x == x - 3, (name, prev_q.x, x)
    prev_q.extend(z)
    b.wire(x - 2, Y, z); b.wire(x - 1, Y, z)
    b.repeater(x, Y, z, 'west', delay=4)
    b.repeater(x + 1, Y, z, 'west')
    b.wire(x + 2, Y, z); b.wire(x + 3, Y, z)
    b.repeater(x + 1, Y, z + 1, 'south')
    b.wtorch(x + 1, Y, z + 2, 'north')
    b.solid(x + 1, Y, z + 3)
    b.wire(x + 3, Y, z + 1)
    q = f.new_lane(name, x + 3, z + 1, 'south', loose=True)
    if write_lane is not None:
        assert write_lane.x == x + 1 and write_lane.flow == 'north'
        write_lane.extend(z + 4)
    return q


class Fifo:
    def __init__(self, f, name, d_lane, stages, x0, z0, far=100000):
        self.f, self.name, self.stages, self.x0, self.z0 = f, name, stages, x0, z0
        self.q = []
        self.w = []
        prev = d_lane
        for k in range(stages):
            x, z = x0 + 6 * k, z0 + 8 * k
            prev = fifo_bit(f, f'{name}.Q{k}', prev, None, x, z)
            self.q.append(prev)
            # the write column: a north-flowing placeholder that a gate joins later
            wl = f.new_lane(f'{name}.W{k}', x + 1, far, flow='north', loose=True)
            wl.extend(z + 4)
            self.w.append(wl)

    def write_slot(self, k):
        return self.x0 + 6 * k + 1

    def attach_write(self, k, write_lane):
        """Legacy: a north-flowing lane created by the caller at slot x+1."""
        x, z = self.x0 + 6 * k, self.z0 + 8 * k
        assert write_lane.x == x + 1 and write_lane.flow == 'north'
        write_lane.extend(z + 4)

    def drive_write(self, k, gate_fn):
        """Create the write gate joined onto the placeholder column k."""
        wl = self.w[k]
        gate_fn(x=wl.x, join=wl)
        wl.z0 = max(wl.taps)                       # the lane really starts at the gate
