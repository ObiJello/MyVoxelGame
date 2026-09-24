"""Builds the redstone computer world.

    python3 make_world.py            # write the world into the obeycraft saves folder
    python3 make_world.py --test 24  # simulate 24 random inputs through the whole machine
"""
import os, random, sys
from build import Builder, Y
from cells import run_wire
from adder import adder, PITCH, Z_LAMP, Z_CARRY
from display import display, FONT
import writer

OX, OZ = 8, 8            # keep everything inside region r.0.0
NAME = 'Redstone Computer'


def machine():
    b = Builder()
    pins = adder(b, OX, 4)
    # sum bits continue south over the carry row into the display
    xs = [OX + i * PITCH + 10 for i in range(4)]
    zd = OZ + 60
    for i in range(4):
        x = xs[i]
        lane_z = zd + 16 * (3 - i)
        b.bridge(x, Z_CARRY, 'z', 'south')                 # cells 45 .. 51
        run_wire(b, [(x, Z_CARRY + 3), (x, lane_z - 1)])
    info = display(b, xs, zd, OX + 110, OX + 208)
    return b, pins, info


def read_digit(w, info):
    return ''.join(s for s in 'abcdefg' if w.lamp(info['lamps'][s]))


def test(n):
    b, pins, info = machine(); w = b.w
    print('blocks', len(w.blocks))
    rng = random.Random(1)
    cases = [(0, 0, 0), (15, 15, 1), (9, 6, 0), (1, 1, 0)] + [
        (rng.randrange(16), rng.randrange(16), rng.randrange(2)) for _ in range(n)]
    fails = 0
    for a, c, cin in cases:
        for i in range(4):
            w.set_lever(pins['A'][i], (a >> i) & 1); w.set_lever(pins['B'][i], (c >> i) & 1)
        w.set_lever(pins['Cin'], cin)
        w.settle()
        s = sum(int(w.lamp(pins['S'][i])) << i for i in range(4))
        cout = int(w.lamp(pins['Cout']))
        digit = read_digit(w, info)
        want = a + c + cin
        ok = (s | (cout << 4)) == want and digit == ''.join(sorted(FONT[want & 15]))
        fails += not ok
        print(f'{a:2d} + {c:2d} + {cin} = {s + 16 * cout:2d}  digit {digit:8s} {"ok" if ok else "FAIL"}')
    print('fails', fails)
    return fails


def write(name=NAME):
    b, pins, info = machine(); w = b.w
    for i in range(4):
        w.set_lever(pins['A'][i], 0); w.set_lever(pins['B'][i], 0)
    w.set_lever(pins['Cin'], 0)
    w.settle()
    saves = os.path.expanduser('~/Library/Application Support/obeycraft/saves')
    folder = os.path.join(saves, name)
    tmpl = os.path.join(saves, 'grass flat (2)')
    n = writer.write_world(w, folder, name, (OX + 4, writer.SURFACE_Y, OZ - 6),
                           os.path.join(tmpl, 'level.dat'),
                           os.path.join(tmpl, 'data', 'obeycraft.json'))
    print('wrote', n, 'chunks,', len(w.blocks), 'blocks to', folder)


if __name__ == '__main__':
    if '--test' in sys.argv:
        sys.exit(1 if test(int(sys.argv[sys.argv.index('--test') + 1])) else 0)
    write(sys.argv[sys.argv.index('--name') + 1] if '--name' in sys.argv else NAME)
