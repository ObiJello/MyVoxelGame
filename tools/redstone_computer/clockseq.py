"""Clock and phase sequencer (floor macros feeding fabric lanes).

oscillator(b, x, z, n)        ring: block+torch, n repeaters on delay 4, wire
                              back into the block. Period = 2*(2 + 8n + 2*r) ticks, r = repeaters on the return row.
                              Returns the CLK wire cell (x+4+n, z+1).
sequencer(f, g_lane, x, z, spacing)
                              g_lane: gated clock lane (flows south, ends at
                              (x, z)). A pulse generator makes P1 on the
                              rising edge; a repeater chain delays it by
                              `spacing` ticks per phase (multiples of 8:
                              spacing//8 repeaters on delay 4). Returns the
                              list of phase lanes P1..P7 (4-tick pulses,
                              active HIGH) at slots 3 apart.
"""
from build import Y
from seq import pulse_gen
from cells import run_wire


def oscillator(b, x, z, n, pause=True, active=None):
    """With `pause`, a lever at (x, z-3) facing south feeds block A through
    (x, z-2), (x, z-1): lever ON powers A, the torch stays off and the ring
    is stopped. Lever OFF runs the clock."""
    if pause:
        b.lever(x, Y, z - 3, 'south'); b.wire(x, Y, z - 2); b.wire(x, Y, z - 1)
    b.solid(x, Y, z); b.wtorch(x + 1, Y, z, 'east'); b.wire(x + 2, Y, z)
    # `active` (redstone_plus): only the first `active` cells are delay-4 repeaters, the
    # rest plain dust (no delay without decay), so the period shrinks while the band's
    # footprint — which the sequencer and its gates are placed against — stays put
    active = n if active is None else active
    for i in range(n):
        if i < active:
            b.repeater(x + 3 + i, Y, z, 'west', delay=4)
        else:
            b.wire(x + 3 + i, Y, z)
    xe = x + 3 + n
    b.wire(xe, Y, z); b.wire(xe, Y, z + 1); b.wire(xe, Y, z + 2)
    cells = list(range(xe - 1, x - 3, -1))          # the return row, in flow order (westwards)
    since = 0
    for i, xx in enumerate(cells):
        if 0 < i < len(cells) - 1 and since >= 8:
            b.repeater(xx, Y, z + 2, 'east'); since = 0   # faces its input (east)
        else:
            b.wire(xx, Y, z + 2); since += 1
    b.wire(x - 2, Y, z + 1); b.wire(x - 2, Y, z); b.wire(x - 1, Y, z)
    b.wire(xe + 1, Y, z + 1)                 # CLK tap off the loop corner
    return (xe + 1, z + 1)


def sequencer(f, g_lane, x, z, phases=7, spacing=16):
    """`spacing` may be an int or a list (ticks before each phase after the
    first); every value is a multiple of 8."""
    """The gated clock LEVEL runs east along z with two delay-4 repeaters per
    phase (levels pass through repeaters unstretched); each phase branches
    south into its own pulse generator, so every P_k is a clean 4-tick pulse
    `spacing` ticks after the previous one. Phase lanes are 12 slots apart."""
    b = f.b
    assert g_lane.x == x and g_lane.flow == 'south'
    g_lane.extend(z)
    lanes = []
    spacings = [spacing] * (phases - 1) if isinstance(spacing, int) else list(spacing)
    assert len(spacings) == phases - 1
    cx = x
    for k in range(phases):
        if k > 0:
            b.wire(cx, Y, z)
        # branch south into the pulse generator (a repeater first: the level
        # row is down to a few levels of power by the end of each segment)
        b.repeater(cx, Y, z + 1, 'north')
        for zz in range(z + 2, z + 6):
            b.wire(cx, Y, zz)
        b.wire(cx + 1, Y, z + 5)
        ox, oz = pulse_gen(b, cx + 2, z + 5)          # out at (cx+9, z+5)
        b.wire(ox, Y, oz + 1)
        lanes.append(f.new_lane(f'P{k + 1}', ox, oz + 1, 'south', loose=True))
        if k + 1 < phases:
            nrep = spacings[k] // 8
            seg = max(12, nrep + 3)
            for i in range(nrep):
                b.repeater(cx + 1 + i, Y, z, 'west', delay=4)
            for xx in range(cx + 1 + nrep, cx + seg):
                b.wire(xx, Y, z)
            cx += seg
    return lanes


def sequencer_width(phases, spacing):
    spacings = [spacing] * (phases - 1) if isinstance(spacing, int) else list(spacing)
    return sum(max(12, s // 8 + 3) for s in spacings) + 12
