import itertools
from build import Builder, Y

def run(b, levers, lamps):
    """Truth table: returns dict input-tuple -> output-tuple."""
    table = {}
    for bits in itertools.product([0, 1], repeat=len(levers)):
        for p, v in zip(levers, bits):
            b.w.set_lever(p, v)
        b.w.settle()
        table[bits] = tuple(int(b.w.lamp(p)) for p in lamps)
    return table

def test_not():
    b = Builder()
    b.lever(0, Y, 0)
    b.line(1, 0, 3, 0)
    b.not_gate(4, 0, 'east')          # torch at (5,Y,0)
    b.lamp(6, Y, 0)
    t = run(b, [(0, Y, 0)], [(6, Y, 0)])
    assert t == {(0,): (1,), (1,): (0,)}, t

def test_and():
    # A at z=0, B at z=2 run east into blocks with torches on top; the torch
    # outputs meet on a wire at y+1 between them which powers the block below,
    # and a wall torch on that block's east face is A AND B.
    b = Builder()
    b.lever(0, Y, 0); b.line(1, 0, 3, 0)
    b.lever(0, Y, 2); b.line(1, 2, 3, 2)
    b.solid(4, Y, 0); b.torch(4, Y + 1, 0)
    b.solid(4, Y, 2); b.torch(4, Y + 1, 2)
    b.solid(4, Y, 1); b.wire(4, Y + 1, 1)
    b.wtorch(5, Y, 1, 'east')
    b.wire(6, Y, 1); b.lamp(7, Y, 1)
    t = run(b, [(0, Y, 0), (0, Y, 2)], [(7, Y, 1)])
    assert t == {(0,0):(0,), (0,1):(0,), (1,0):(0,), (1,1):(1,)}, t

def test_xor_comparators():
    # A lane z=0 (east-west), B lane z=8. C1 rear=A side=B; C2 rear=B side=A.
    b = Builder()
    b.lever(0, Y, 0); b.line(1, 0, 20, 0)
    b.lever(0, Y, 8); b.line(1, 8, 20, 8)
    x0 = 6
    b.repeater(x0, Y, 1, 'north'); b.comparator(x0, Y, 2, 'north', 'subtract')
    b.wire(x0, Y, 3)
    b.repeater(x0 - 1, Y, 2, 'west'); b.line(x0 - 2, 2, x0 - 2, 7)     # B tap
    b.repeater(x0 + 4, Y, 7, 'south'); b.comparator(x0 + 4, Y, 6, 'south', 'subtract')
    b.wire(x0 + 4, Y, 5)
    b.repeater(x0 + 5, Y, 6, 'east'); b.line(x0 + 6, 6, x0 + 6, 1)     # A tap
    b.path([(x0, 3), (x0 + 2, 3), (x0 + 2, 5), (x0 + 4, 5)])           # OR
    b.solid(x0 + 2, Y, 4)  # (already wire) - noop guard
    b.wire(x0 + 2, Y, 4)
    b.lamp(x0 + 3, Y + 1, 4)
    # take output up: wire at (x0+2,4) -> lamp above? simpler: lamp adjacent to OR wire
    b.lamp(x0 + 1, Y, 4)
    t = run(b, [(0, Y, 0), (0, Y, 8)], [(x0 + 1, Y, 4)])
    assert t == {(0,0):(0,), (0,1):(1,), (1,0):(1,), (1,1):(0,)}, t

def test_bridge():
    # signal S runs along z at x=5 crossing lane L (along x at z=5).
    b = Builder()
    b.lever(0, Y, 5); b.line(1, 5, 10, 5); b.lamp(11, Y, 5)
    b.lever(5, Y, 0); b.line(5, 1, 5, 1); b.bridge(5, 5, 'z', 'south'); b.line(5, 9, 5, 9); b.lamp(5, Y, 10)
    t = run(b, [(0, Y, 5), (5, Y, 0)], [(11, Y, 5), (5, Y, 10)])
    for (l, s), (ol, os_) in t.items():
        assert ol == l and os_ == s, t

for f in (test_not, test_and, test_bridge):   # the comparator XOR lives in test_cells
    f(); print('ok', f.__name__)
