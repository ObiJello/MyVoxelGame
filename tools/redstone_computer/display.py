"""Hex display: a 4-to-16 decoder, a 16x7 diode-matrix ROM holding the
seven-segment font, and a 21x49 lamp digit.

Layout (x east, z south), all relative to the decoder origin (0, ZD):
  input columns   S3..S0 arrive from the north at x = XS[i] and each starts a
                  pair of lanes running east: S_i at z = ZD + 16*(3-i) and
                  ~S_i (a torch NOT) 8 further south. Ordering the pairs by
                  where their columns arrive means no column crosses a lane.
  minterm columns 16 columns at x = XM0 + 6*m running south over every lane
                  on bridges. A column collects (repeater taps) the four lanes
                  that must be LOW for its value m; a torch at the bottom
                  turns that OR into "input == m".
  ROM             7 segment lanes at z = ZR + 8*j, order a b f g c e d, run
                  east under the columns; a column taps (repeater) every lane
                  whose segment the font lights for its value.
  digit           lamps under wire, fed from the lane ends; b and c bridge
                  over the f and e columns.
"""
from build import Builder, Y
from cells import run_wire

SEG_ORDER = ['a', 'b', 'f', 'g', 'c', 'e', 'd']
FONT = {
    0: 'abcdef', 1: 'bc', 2: 'abdeg', 3: 'abcdg', 4: 'bcfg', 5: 'acdfg',
    6: 'acdefg', 7: 'abc', 8: 'abcdefg', 9: 'abcdfg', 10: 'abcefg', 11: 'cdefg',
    12: 'adef', 13: 'bcdeg', 14: 'adefg', 15: 'aefg',
}
LANE_PITCH = 8
COL_PITCH = 6


def display(b, xs, zd, xm0, xd):
    """xs: x of the four input columns (index = bit); each column must already
    end at (xs[i], zd_i - 1) coming from the north, where zd_i is the S_i lane.
    Returns dict with 'lamps' (segment -> a lamp position) and 'lanes'."""
    lane_end = xm0 + COL_PITCH * 15 + 3
    lane_z = {}
    for i in range(4):
        z = zd + 16 * (3 - i)
        lane_z[('S', i)] = z
        lane_z[('N', i)] = z + 8
    cols = [xm0 + COL_PITCH * m for m in range(16)]
    avoid = set()
    for xc in cols:
        for z in lane_z.values():
            avoid.update({(xc - 2, z), (xc - 1, z), (xc, z), (xc + 1, z), (xc + 2, z)})

    # input lanes + NOT lanes
    for i in range(4):
        x = xs[i]; zs = lane_z[('S', i)]; zn = lane_z[('N', i)]
        b.wire(x, Y, zs)                                   # column end / lane start
        run_wire(b, [(x, zs), (lane_end, zs)], avoid=avoid)
        b.line(x, zs + 1, x, zs + 5)                       # continue south into the NOT
        b.solid(x, Y, zs + 6)
        b.wtorch(x, Y, zs + 7, 'south')
        run_wire(b, [(x, zn), (lane_end, zn)], avoid=avoid)

    # ROM lanes
    zr = zd + 68
    rom_z = {seg: zr + LANE_PITCH * j for j, seg in enumerate(SEG_ORDER)}
    rom_avoid = set()
    for xc in cols:
        for z in rom_z.values():
            rom_avoid.update({(xc - 1, z), (xc, z), (xc + 1, z), (xc + 2, z), (xc + 3, z)})
    for seg in SEG_ORDER:
        run_wire(b, [(xm0 - 2, rom_z[seg]), (xd - 4, rom_z[seg])], avoid=rom_avoid)

    # minterm columns
    all_lanes = sorted(lane_z.values())
    for m, xc in enumerate(cols):
        # decoder part: top plain cell, then a bridge over every lane
        b.wire(xc, Y, all_lanes[0] - 4)
        for z in all_lanes:
            b.bridge(xc, z, 'z', 'south')                  # cells z-3 .. z+3
        for k in range(len(all_lanes) - 1):
            b.wire(xc, Y, all_lanes[k] + 4)                # plain cell between lanes
        # taps: the lane that must be low for bit i
        for i in range(4):
            z = lane_z[('N', i)] if (m >> i) & 1 else lane_z[('S', i)]
            b.path([(xc - 2, z - 1), (xc - 2, z - 4)])
            b.repeater(xc - 1, Y, z - 4, 'west')
        # NOT at the bottom -> minterm
        zl = all_lanes[-1]
        b.wire(xc, Y, zl + 4)
        b.solid(xc, Y, zl + 5)
        b.wtorch(xc, Y, zl + 6, 'south')
        b.wire(xc, Y, zl + 7)
        b.wire(xc, Y, zl + 8)                               # = zr - 4 (plain cell)
        # ROM part: bridges over the segment lanes with taps
        rz = [rom_z[s] for s in SEG_ORDER]
        for z in rz:
            b.bridge(xc, z, 'z', 'south')
        for k in range(len(rz) - 1):
            b.wire(xc, Y, rz[k] + 4)
        for seg in SEG_ORDER:
            if seg in FONT[m]:
                z = rom_z[seg]
                b.path([(xc + 1, z - 4), (xc + 2, z - 4), (xc + 2, z - 2)])
                b.repeater(xc + 2, Y, z - 1, 'north')

    # digit: wires on lamps. Every segment is fed through a repeater so it
    # starts at 15 where it enters: the horizontals are 15 lamps fed from the
    # end, the verticals 21 lamps fed 14 from their far end (1 is still lit).
    lamps = {}
    def seg_line(x0, z0, x1, z1):
        first = None
        if x0 == x1:
            for z in range(min(z0, z1), max(z0, z1) + 1):
                b.lamp(x0, Y - 1, z); b.wire(x0, Y, z)
                first = first or (x0, Y - 1, z)
        else:
            for x in range(min(x0, x1), max(x0, x1) + 1):
                b.lamp(x, Y - 1, z0); b.wire(x, Y, z0)
                first = first or (x, Y - 1, z0)
        return first
    za, zg, zdd = rom_z['a'], rom_z['g'], rom_z['d']
    W = 20
    for seg in ('a', 'g', 'd'):
        z = rom_z[seg]
        b.line(xd - 3, z, xd + 1, z)
        b.repeater(xd + 2, Y, z, 'west')
        lamps[seg] = seg_line(xd + 3, z, xd + W - 3, z)
    for seg in ('f', 'e'):
        z = rom_z[seg]
        b.line(xd - 3, z, xd - 2, z)
        b.repeater(xd - 1, Y, z, 'west')
    lamps['f'] = seg_line(xd, za + 2, xd, zg - 2)
    lamps['e'] = seg_line(xd, zg + 2, xd, zdd - 2)
    for seg in ('b', 'c'):
        z = rom_z[seg]
        b.bridge(xd, z, 'x', 'east')                        # cells xd-3 .. xd+3
        run_wire(b, [(xd + 4, z), (xd + W - 2, z)])
        b.repeater(xd + W - 1, Y, z, 'west')
    lamps['b'] = seg_line(xd + W, za + 2, xd + W, zg - 2)
    lamps['c'] = seg_line(xd + W, zg + 2, xd + W, zdd - 2)
    # report the far ends of the verticals (weakest lamp) for the tests
    lamps['f'] = (xd, Y - 1, za + 2); lamps['e'] = (xd, Y - 1, zg + 2)
    lamps['b'] = (xd + W, Y - 1, zg - 2); lamps['c'] = (xd + W, Y - 1, zdd - 2)
    lamps['a'] = (xd + W - 3, Y - 1, za); lamps['g'] = (xd + W - 3, Y - 1, zg); lamps['d'] = (xd + W - 3, Y - 1, zdd)
    return {'lamps': lamps, 'rom_z': rom_z, 'lane_z': lane_z}
