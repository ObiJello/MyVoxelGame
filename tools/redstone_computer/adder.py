"""The 4-bit ripple-carry adder: four identical full-adder slices side by side.

Slice frame (relative x, z; x east, z south), wire level Y:
  columns  B x=0 (lever at z=0), A x=10 (lever at z=0)
  AND1     G = A·B      between B and A, zm=6, output north then a bridge
                        east over column A at z=2, then east along z=2
  XOR1     P = A^B      between B and A, z0=14, exits south over its tap
                        row at x=5 -> column P (x=5) from z=23
  AND2     H = P·C      between P (x=5) and C (x=15), zm=26, output north
                        along x=12 then east along z=4
  XOR2     S = P^C      between P and C, z0=33, exits south at x=10 -> lamp
  OR       Cout = G + H at x=22 (two repeaters as diodes), column x=22
                        south to the carry row z=48, east into the next slice
  C in     carry row z=48 from the west edge to x=15, column C rises to z=29
"""
from build import Builder, Y
from cells import run_wire, xor_cell, xor_exit_south, and_cell

PITCH = 26
Z_LAMP = 44
Z_CARRY = 48


def slice_(b, ox, i):
    def X(x): return ox + x
    # levers and columns
    b.lever(X(0), Y, 0, 'south'); b.lever(X(10), Y, 0, 'south')
    # cells other circuitry reads sideways must stay plain wire
    run_wire(b, [(X(0), 1), (X(0), 20)], avoid={(X(0), 3), (X(0), 14), (X(0), 20)})
    run_wire(b, [(X(10), 1), (X(10), 18)], avoid={(X(10), 9), (X(10), 12), (X(10), 18)})
    # AND1 -> G
    gx, gz = and_cell(b, X(0), X(10), 6)               # (6, 6)
    b.line(gx, gz, gx, 2)                                # north to z=2
    b.bridge(X(10), 2, 'x', 'east')                      # cells x=7..13 at z=2
    run_wire(b, [(X(13), 2), (X(20), 2)])
    b.repeater(X(21), Y, 2, 'west')
    # XOR1 -> P
    xor_cell(b, X(0), X(10), 14)
    px, pz = xor_exit_south(b, X(0), X(10), 14)          # (5, 23)
    run_wire(b, [(px, pz), (px, 39)], avoid={(px, 23), (px, 33), (px, 39)})
    # AND2 -> H
    hx, hz = and_cell(b, px, X(15), 26)                  # (11, 26)
    run_wire(b, [(hx, hz), (hx + 1, hz), (hx + 1, 4), (X(20), 4)])
    b.repeater(X(21), Y, 4, 'west')
    # XOR2 -> S -> lamp
    xor_cell(b, px, X(15), 33)
    sx, sz = xor_exit_south(b, px, X(15), 33)            # (10, 42)
    b.line(sx, sz, sx, Z_LAMP - 1)
    b.lamp(sx, Y, Z_LAMP)
    # OR -> Cout column -> carry row east
    b.path([(X(22), 2), (X(22), 4)])
    run_wire(b, [(X(22), 4), (X(22), Z_CARRY), (X(PITCH) - 1, Z_CARRY)])
    # C in: carry row from the west edge, column up to AND2's tap row
    run_wire(b, [(X(0), Z_CARRY), (X(15), Z_CARRY), (X(15), 29)],
             avoid={(X(15), 29), (X(15), 31), (X(15), 37)})
    return {
        'A': (X(10), Y, 0), 'B': (X(0), Y, 0), 'S': (sx, Y, Z_LAMP),
    }


def adder(b, ox=0, bits=4):
    pins = {'A': [], 'B': [], 'S': []}
    b.lever(ox - 3, Y, Z_CARRY, 'south')                  # carry-in lever
    b.line(ox - 2, Z_CARRY, ox - 1, Z_CARRY)
    for i in range(bits):
        p = slice_(b, ox + i * PITCH, i)
        for k in pins: pins[k].append(p[k])
    xe = ox + bits * PITCH
    b.line(xe, Z_CARRY, xe + 1, Z_CARRY)
    b.lamp(xe + 2, Y, Z_CARRY)
    pins['Cin'] = (ox - 3, Y, Z_CARRY)
    pins['Cout'] = (xe + 2, Y, Z_CARRY)
    return pins
