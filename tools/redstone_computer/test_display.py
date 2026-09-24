import sys
from build import Builder, Y, dump
from display import display, FONT

def build():
    b = Builder()
    xs = [18, 44, 70, 96]          # S0..S3 column x
    zd = 60
    levers = []
    for i in range(4):
        z = zd + 16 * (3 - i)
        b.lever(xs[i], Y, z - 3, 'south'); b.line(xs[i], z - 2, xs[i], z - 1)
        levers.append((xs[i], Y, z - 3))
    info = display(b, xs, zd, 118, 216)
    return b, levers, info

if __name__ == '__main__':
    b, levers, info = build()
    w = b.w
    fails = 0
    vals = range(16) if '--all' in sys.argv else [0, 1, 5, 8, 10, 15]
    for v in vals:
        for i in range(4): w.set_lever(levers[i], (v >> i) & 1)
        w.settle()
        lit = ''.join(s for s in 'abcdefg' if w.lamp(info['lamps'][s]))
        ok = lit == ''.join(sorted(FONT[v]))
        fails += not ok
        print(f'{v:2d}: {lit:8s} {"ok" if ok else "EXPECTED " + FONT[v]}')
    print('blocks', len(w.blocks), 'fails', fails)
