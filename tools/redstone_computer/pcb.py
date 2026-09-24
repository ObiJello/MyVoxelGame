"""Two-layer wiring scheme ("PCB") for the machine.

  Layer 0  (level Y, on the floor)      LANES: long wires running north-south,
                                        one signal each, x pitch 3.
  Layer 2  (level Y+2, on glowstone)    ROWS: wires running east-west, z pitch
                                        2, crossing over lanes freely. Gates
                                        (solid block + wall torch) live here.
  Vias     a 2-cell ramp: lane cell (x, Y) <-> wire (x+1, Y+1) on a SOLID
           block <-> row cell (x+2, Y+2) on glowstone. Ramps always sit on the
           EAST side of a lane; the cell x+2 at floor level stays empty and
           the next lane is at x+3, so a ramp only touches its own lane.

Glowstone under the upper layer is what keeps the layers apart: it is not a
redstone conductor, so an upper wire cannot power it and a lane below cannot
read through it. Every gate block is a normal solid block; its inputs are
upper-layer wires pointing straight into it (or lying on top of it), which
give it strong power, and the torch on its output face is the NOR of them.
"""
import build
from build import Builder, Y
from cells import run_wire
from redsim import DIRS, OPP

UP = Y + 2      # upper wire level
MID = Y + 1     # glowstone / ramp level


class Pcb:
    def __init__(self, b):
        self.b = b

    # ── layer 0 ─────────────────────────────────────────────────────────
    def lane(self, x, z0, z1, avoid=(), rep_every=8):
        """Lane along z (south-positive). Repeaters face against the flow, so
        pass z0 = source end."""
        run_wire(self.b, [(x, z0), (x, z1)], y=Y, rep_every=rep_every, avoid=set(avoid))

    # ── layer 2 ─────────────────────────────────────────────────────────
    def upper_wire(self, x, z):
        self.b.glow(x, MID, z)
        self.b.wire(x, UP, z)

    def upper_repeater(self, x, z, facing, delay=1):
        self.b.glow(x, MID, z)
        self.b.repeater(x, UP, z, facing, delay)

    def row(self, x0, x1, z, rep_every=12, avoid=()):
        """Row along x on the upper layer; x0 = source end. Repeaters are
        spread EVENLY so that no segment (including the last one, which has
        to drive a gate face or a turn) is longer than rep_every cells: a
        run of n cells gets ceil(n / rep_every) segments."""
        step = 1 if x1 >= x0 else -1
        back = 'west' if step > 0 else 'east'
        cells = list(range(x0, x1 + step, step))
        n = len(cells)
        segs = 1 if build.PLUS else max(1, -(-n // rep_every))
        # repeater indices: boundaries between segments (never the first cell)
        bounds = {round(i * n / segs) for i in range(1, segs)}
        for i, x in enumerate(cells):
            if i in bounds and i < n - 1 and (x, z) not in avoid:
                self.upper_repeater(x, z, back)
            else:
                self.upper_wire(x, z)

    def upper_path(self, pts, rep_every=8):
        """Polyline on the upper layer."""
        for (x0, z0), (x1, z1) in zip(pts, pts[1:]):
            if x0 == x1:
                zs = range(z0, z1 + (1 if z1 >= z0 else -1), 1 if z1 >= z0 else -1)
                for z in zs: self.upper_wire(x0, z)
            else:
                self.row(x0, x1, z0, rep_every=rep_every)

    # ── vias ────────────────────────────────────────────────────────────
    def via_up(self, x, z):
        """Ramp from lane (x, z) up to the upper layer, followed by a repeater
        at (x+3, z) that refreshes the tapped signal to 15 (a lane is at
        whatever strength it has by then). The row starts at (x+4, z). The
        wire at (x+1, MID) sits on a solid block at floor level, and so does
        the upper cell (x+2, UP): under glowstone, a floor wire at (x+2, Y)
        (a lane one column off the slot grid, e.g. a register's write
        column) would climb the solid at (x+1, Y) into the ramp and short
        the two lanes together. A conductor above it stops any climb."""
        self.b.solid(x + 1, Y, z)
        self.b.wire(x + 1, MID, z)
        self.b.solid(x + 2, MID, z)          # solid, not glowstone: see via_down
        self.b.wire(x + 2, UP, z)
        self.upper_repeater(x + 3, z, 'west')
        return (x + 4, z)

    def via_up_west(self, x, z):
        """Mirror of via_up: ramp on the WEST side of lane (x, z); the row
        starts at (x-4, z) heading west (repeater at x-3 faces east)."""
        self.b.solid(x - 1, Y, z)
        self.b.wire(x - 1, MID, z)
        self.b.solid(x - 2, MID, z)          # solid, not glowstone: see via_down
        self.b.wire(x - 2, UP, z)
        self.upper_repeater(x - 3, z, 'east')
        return (x - 4, z)

    def via_down_west(self, x, z):
        """Mirror of via_down: upper cell (x-2, z), reached from the WEST,
        drops to lane cell (x, z). (x-2, Y, z) must stay empty."""
        self.b.solid(x - 1, Y, z)
        self.b.wire(x - 1, MID, z)
        self.b.solid(x - 2, MID, z)
        self.b.wire(x - 2, UP, z)
        return (x, z)

    def via_down(self, x, z):
        """Ramp used downwards: upper cell (x+2, z) -> lane cell (x, z). The
        upper end cell sits on a SOLID block, not glowstone: dust reads the
        wire diagonally above it only through a conductor (glowstone lets a
        signal climb but never descend). That solid is powered by the wire
        on it, so the floor cell under it, (x+2, Y, z), must stay empty."""
        self.b.solid(x + 1, Y, z)
        self.b.wire(x + 1, MID, z)
        self.b.solid(x + 2, MID, z)
        self.b.wire(x + 2, UP, z)
        return (x, z)

    # ── gates ───────────────────────────────────────────────────────────
    def gate(self, x, z, out):
        """NOR block at (x, UP, z) with a torch on face `out`. Returns the
        torch's output cell (x', z') on the upper layer and the three input
        cells (the cell adjacent to each free side face)."""
        b = self.b
        b.glow(x, MID, z)
        b.solid(x, UP, z)
        d = DIRS[out]
        tx, tz = x + d[0], z + d[2]
        b.glow(tx, MID, tz)                      # keep the torch cell's support consistent
        b.wtorch(tx, UP, tz, out)
        faces = {}
        for f in ('north', 'south', 'east', 'west'):
            if f == out: continue
            fd = DIRS[f]
            faces[f] = (x + fd[0], z + fd[2])
        return (tx, tz), faces

    def gate_input(self, cell, frm, length=2):
        """Straight upper wire of `length` cells ending at `cell`, arriving
        from `frm`, so it points into the gate block beyond. Returns far end."""
        d = DIRS[frm]
        pts = [(cell[0] + d[0] * i, cell[1] + d[2] * i) for i in range(length)]
        for (x, z) in pts:
            self.upper_wire(x, z)
        return pts[-1]

    def torch_out(self, cell, out, length=1):
        """Wire cells after a gate's torch, continuing in direction `out`."""
        d = DIRS[out]
        last = cell
        for i in range(1, length + 1):
            last = (cell[0] + d[0] * i, cell[1] + d[2] * i)
            self.upper_wire(*last)
        return last
