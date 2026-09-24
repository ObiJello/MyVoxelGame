"""Clocked primitives, each a fixed footprint with named ports.

Timing conventions (game ticks; a repeater on delay n is 2n ticks, a torch 2):
  pulse_gen   rising edge on IN -> OUT high for 4 ticks, from +4 to +8.
  latch       Q = D while the WRITE input is high (through a torch inverter and
              the side-lock repeater, the repeater is unlocked from +4 to +8
              after WRITE rises), held otherwise. Fed a pulse_gen output it is
              a write-on-edge register bit.
  tflop       a latch with D = NOT Q; every pulse toggles Q.
All cells sit at wire level Y with their own floor; x east, z south.
"""
from build import Builder, Y
from redsim import OPP


def pulse_gen(b, x, z):
    """Input wire at (x, z) arriving from the west. Output wire leaves (x+7, z)
    eastwards. Footprint x..x+7, z-2..z+1.

        (x,z) IN --> block A (x+1,z) with torch NOT A on top at y+1
                 \\-> repeater delay 3 at (x+1,z+1)?  -- laid out below
    Layout: IN cell (x,z); repeater d3 at (x+1,z) facing west feeds block C
    at (x+2,z)... we need NOT A and delayed A both into ONE block.
    """
    # in cell
    b.wire(x, Y, z)
    # branch north: NOT A -> torch on block (x, z-2)
    b.wire(x, Y, z - 1)
    b.solid(x, Y, z - 2); b.torch(x, Y + 1, z - 2)          # NOT A at y+1
    b.wire(x + 1, Y + 1, z - 2)                             # NOT A wire (on a block)
    b.solid(x + 1, Y, z - 2)
    # branch east: A delayed 6 ticks
    b.repeater(x + 1, Y, z, 'west', delay=3)
    b.wire(x + 2, Y, z)
    # both into block (x+3, z-1): NOT A wire steps down onto it from (x+2,z-2)->? use L-turns
    b.wire(x + 2, Y + 1, z - 2)
    b.solid(x + 2, Y, z - 2)
    b.wire(x + 3, Y + 1, z - 2)
    b.solid(x + 3, Y, z - 2)
    b.wire(x + 3, Y + 1, z - 1)                             # points south into ... need a block
    b.solid(x + 3, Y, z - 1)
    # the delayed A: (x+2,z) -> (x+3,z) wire pointing north into block (x+3,z-1)? a wire
    # from the west turning north: make (x+3, z) the end cell with only a west neighbour
    b.wire(x + 3, Y, z)
    # Block (x+3, z-1) is powered by: NOT-A wire ABOVE it (x+3, y+1, z-1) (wire on top powers
    # the block below) and the delayed-A wire (x+3, z) pointing north into it. Torch on its
    # east face = NOT(NOT A or Adel) = A and not Adel.
    b.wtorch(x + 4, Y, z - 1, 'east')
    b.wire(x + 5, Y, z - 1)
    b.wire(x + 5, Y, z)
    b.wire(x + 6, Y, z)
    b.wire(x + 7, Y, z)
    return (x + 7, z)


def latch(b, x, z, d_from_west=True):
    """Repeater-lock latch. D arrives at (x, z) from the west; Q leaves from
    (x+3, z) eastwards. WRITE arrives at (x+1, z+4) from the south (a wire).
    Footprint x..x+3, z..z+4."""
    b.wire(x, Y, z)
    b.repeater(x + 1, Y, z, 'west')                  # the storage repeater R
    b.wire(x + 2, Y, z); b.wire(x + 3, Y, z)
    # lock chain: WRITE -> block -> torch (NOT WRITE) -> repeater into R's south side
    b.wire(x + 1, Y, z + 4)                          # WRITE in
    b.solid(x + 1, Y, z + 3)
    b.wtorch(x + 1, Y, z + 2, 'north')               # NOT WRITE, points north
    b.repeater(x + 1, Y, z + 1, 'south')             # lock repeater: input from south, into R
    return (x + 3, z)


def tflop(b, x, z):
    """Toggle flip-flop: pulse in at (x+4, z+8) from the south (the latch's
    WRITE port), Q out east at (x+6, z), NOT Q available at (x+8, z+3)? We keep
    it simple: Q at (x+7, z). Footprint x..x+8, z-3..z+8."""
    # latch at (x+3, z): D from (x+3,z), R at (x+4,z), Q at (x+6,z), WRITE at (x+4, z+4)
    latch(b, x + 3, z)
    # feedback: Q -> block -> torch = NOT Q -> back to D (west side), routed north
    b.wire(x + 7, Y, z)
    b.line(x + 7, z - 1, x + 7, z - 2)
    b.line(x + 1, z - 3, x + 7, z - 3)
    b.wire(x + 1, Y, z - 2)                          # points south into block (x+1, z-1)
    b.solid(x + 1, Y, z - 1)
    b.wtorch(x + 1, Y, z, 'south')                   # NOT Q, points south -> (x+1, z+1)
    b.wire(x + 1, Y, z + 1)
    b.wire(x + 2, Y, z + 1)
    b.wire(x + 3, Y, z + 1)                          # joins D at (x+3, z) from the south
    b.wire(x, Y, z + 1); b.wire(x - 1, Y, z + 1); b.wire(x - 2, Y, z + 1)   # NOT Q tap, leaves westwards
    return {'Q': (x + 7, z), 'WRITE': (x + 4, z + 4), 'NOTQ': (x - 2, z + 1)}


def counter_up(b, x, z, bits, lamps=True):
    """Ripple up-counter of `bits` toggle flip-flops along +x (pitch 18, so
    that with x = 2 (mod 3) every port lane sits on a multiple of 3).
    Count pulses arrive at (x-3, z+8) from the west (a pulse_gen input).
    Stage i's NOT Q triggers stage i+1's pulse generator, so a stage toggles
    when the previous Q falls. Returns {'IN': (x-3, z+8), 'Q': [(x, z)...]}."""
    from cells import run_wire
    qs = []
    for i in range(bits):
        sx = x + 18 * i
        t = tflop(b, sx, z)
        pulse_gen(b, sx - 3, z + 8)                     # out at (sx+4, z+8)
        b.line(sx + 4, z + 5, sx + 4, z + 7)            # up to WRITE (sx+4, z+4)
        qs.append(t['Q'])
        if lamps:
            b.lamp(t['Q'][0], Y - 1, t['Q'][1])
        if i + 1 < bits:
            nx = sx + 18
            # one cell of clearance from the next pulse generator's wires
            run_wire(b, [(sx - 2, z + 1), (sx - 2, z - 5), (nx - 5, z - 5),
                         (nx - 5, z + 8), (nx - 3, z + 8)])
    return {'IN': (x - 3, z + 8), 'Q': qs}
