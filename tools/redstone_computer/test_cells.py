import itertools
from build import Builder, Y
from cells import *

def table(b, levers, lamps):
    t = {}
    for bits in itertools.product([0, 1], repeat=len(levers)):
        for p, v in zip(levers, bits): b.w.set_lever(p, v)
        b.w.settle()
        t[bits] = tuple(int(b.w.lamp(p)) for p in lamps)
    return t

def test_xor_columns():
    b = Builder()
    u, v = 0, 10
    taps = {(u, 16), (v, 8), (u, 10), (v, 14)}
    b.lever(u, Y, 0); run_wire(b, [(u, 1), (u, 30)], avoid=taps)
    b.lever(v, Y, 0); run_wire(b, [(v, 1), (v, 30)], avoid=taps)
    xor_cell(b, u, v, 10)
    x, z = xor_exit_south(b, u, v, 10)
    b.line(x, z, x, z + 2); b.lamp(x, Y - 1, z + 2)
    t = table(b, [(u, Y, 0), (v, Y, 0)], [(x, Y - 1, z + 2)])
    assert t == {(0,0):(0,), (0,1):(1,), (1,0):(1,), (1,1):(0,)}, t

def test_and_columns():
    b = Builder()
    u, v = 0, 10
    taps = {(u, 5), (v, 11)}
    b.lever(u, Y, 0); run_wire(b, [(u, 1), (u, 20)], avoid=taps)
    b.lever(v, Y, 0); run_wire(b, [(v, 1), (v, 20)], avoid=taps)
    ox, oz = and_cell(b, u, v, 8)
    b.lamp(ox, Y - 1, oz)
    t = table(b, [(u, Y, 0), (v, Y, 0)], [(ox, Y - 1, oz)])
    assert t == {(0,0):(0,), (0,1):(0,), (1,0):(0,), (1,1):(1,)}, t

for f in (test_xor_columns, test_and_columns):
    f(); print('ok', f.__name__)
