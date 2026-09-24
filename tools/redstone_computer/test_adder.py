import itertools, sys
from build import Builder, Y, dump
from adder import adder

def check(bits):
    b = Builder(); pins = adder(b, 0, bits); w = b.w
    fails = 0; n = 0
    for a in range(2 ** bits):
        for c in range(2 ** bits):
            for cin in (0, 1):
                for i in range(bits):
                    w.set_lever(pins['A'][i], (a >> i) & 1)
                    w.set_lever(pins['B'][i], (c >> i) & 1)
                w.set_lever(pins['Cin'], cin)
                w.settle()
                s = sum(int(w.lamp(pins['S'][i])) << i for i in range(bits))
                s |= int(w.lamp(pins['Cout'])) << bits
                n += 1
                if s != a + c + cin:
                    fails += 1
                    if fails <= 5: print('FAIL', a, '+', c, '+', cin, '=', s)
    print(f'{bits}-bit: {n} cases, {fails} failures, {len(w.blocks)} blocks')
    return b

if __name__ == '__main__':
    bits = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    b = check(bits)
    if '--dump' in sys.argv:
        for y in (Y, Y + 1, Y + 2):
            print('--- level', y); dump(b, y)
