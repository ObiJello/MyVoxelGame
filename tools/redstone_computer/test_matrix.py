"""Board + select matrices, driven by levers on the register lines."""
from build import Builder
from redtick import TickWorld
from board import board, N, SIZE
from matrix import xmatrix, ymatrix
from pixel import W, L0, L2, L4

BITS = ['Q0', 'NQ0', 'Q1', 'NQ1', 'Q2', 'NQ2']


def bits_for(v):
    return ['NQ%d' % b if (v >> b) & 1 else 'Q%d' % b for b in range(3)]


def build():
    b = Builder(TickWorld()); w = b.w
    P = board(b, 0, 0)
    lv = {}
    # X matrix: three groups of six lines north of the board
    groups = []
    zline = -8
    for g, key in (('HX', 'HX'), ('TX', 'TX'), ('FX', 'FX')):
        lines = {}
        for n in BITS:
            lines[n] = zline
            b.solid(-9, 3, zline); b.lever(-9, 4, zline, 'east')
            b.glow(-8, 3, zline); b.wire(-8, 4, zline); b.glow(-7, 3, zline); b.wire(-7, 4, zline)
            lv[(g, n)] = (-9, 4, zline)
            zline -= 4
        cols = [(P[key][i][0], bits_for(i)) for i in range(N)]
        groups.append((g, lines, cols))
    xmatrix(b, 0, 0, groups)
    # Y matrix: three groups of six lines plus three pulse lines west of the board
    ylines = {}
    x = -12
    for g in ('HY', 'TY', 'FY'):
        for n in BITS:
            ylines[g + n] = x
            b.lever(x, L0, -5, 'south'); b.wire(x, L0, -4); b.wire(x, L0, -3)
            lv[(g, n)] = (x, L0, -5)
            x -= 6
    for pn in ('PSET', 'PCHECK', 'PERASE'):
        ylines[pn] = x
        b.lever(x, L0, -5, 'south'); b.wire(x, L0, -4); b.wire(x, L0, -3)
        lv[(pn, 0)] = (x, L0, -5)
        x -= 6
    rows = []
    for j in range(N):
        rows.append((16 * j + 0, ['HY' + n for n in bits_for(j)] + ['PSET']))
        rows.append((16 * j + 8, ['HY' + n for n in bits_for(j)] + ['PCHECK']))
        rows.append((16 * j + 10, ['TY' + n for n in bits_for(j)] + ['PERASE']))
        rows.append((16 * j + 14, ['FY' + n for n in bits_for(j)]))
    ymatrix(b, 0, 0, ylines, rows, -2)
    for i, (x, y, z) in enumerate(P['COL']):
        b.lamp(x, y, z + 1)
    assert not b.collisions, b.collisions[:10]
    return b, w, P, lv


def test():
    b, w, P, lv = build()
    for k in lv:
        w.toggle_lever(lv[k], False)          # every line low (undefined registers)
    for pn in ('PSET', 'PCHECK', 'PERASE'):
        w.toggle_lever(lv[(pn, 0)], True)     # pulse lines idle HIGH
    w.boot(); w.run_until_idle(limit=20000)

    def setval(g, v):
        for bt in range(3):
            on = (v >> bt) & 1
            w.toggle_lever(lv[(g, 'Q%d' % bt)], bool(on))
            w.toggle_lever(lv[(g, 'NQ%d' % bt)], not on)

    def pulse(pn, ticks=4):
        w.toggle_lever(lv[(pn, 0)], False); w.tick(ticks); w.toggle_lever(lv[(pn, 0)], True)

    def settle():
        w.run_until_idle(limit=20000)

    body = lambda i, j: w.lamp(P['body'][(i, j)]); food = lambda i, j: w.lamp(P['food'][(i, j)])
    lit = lambda: {(i, j) for i in range(N) for j in range(N) if body(i, j)}
    foods = lambda: {(i, j) for i in range(N) for j in range(N) if food(i, j)}
    col = lambda i: w.lamp((P['COL'][i][0], L4, P['COL'][i][2] + 1))
    print('blocks', len(w.blocks))
    # clear the board: erase every cell
    for i in range(N):
        setval('TX', i)
        for j in range(N):
            setval('TY', j); settle(); pulse('PERASE'); settle()
    assert not lit(), sorted(lit())
    # set a few cells through the decoders
    for (i, j) in ((2, 3), (7, 7), (0, 0), (5, 3), (7, 0), (0, 7)):
        setval('HX', i); setval('HY', j); settle(); pulse('PSET'); settle()
    assert lit() == {(2, 3), (7, 7), (0, 0), (5, 3), (7, 0), (0, 7)}, sorted(lit())
    # checks
    def check(i, j):
        setval('HX', i); setval('HY', j); settle()
        w.toggle_lever(lv[('PCHECK', 0)], False)
        hits = set()
        for _ in range(80):
            w.tick(); hits |= {c for c in range(N) if col(c)}
        w.toggle_lever(lv[('PCHECK', 0)], True); settle()
        return hits
    assert check(2, 3) == {2}, check(2, 3)
    assert check(3, 3) == set()
    assert check(7, 7) == {7}
    assert check(7, 6) == set()
    assert check(0, 7) == {0}
    assert check(7, 0) == {7}
    assert lit() == {(2, 3), (7, 7), (0, 0), (5, 3), (7, 0), (0, 7)}
    # erase two
    for (i, j) in ((2, 3), (0, 7)):
        setval('TX', i); setval('TY', j); settle(); pulse('PERASE'); settle()
    assert lit() == {(7, 7), (0, 0), (5, 3), (7, 0)}, sorted(lit())
    # food
    for (i, j) in ((6, 1), (0, 7), (7, 7), (3, 4)):
        setval('FX', i); setval('FY', j); settle()
        assert foods() == {(i, j)}, (i, j, sorted(foods()))
    # latency: PSET lever -> far pixel set; PCHECK lever -> OR lamp for (7,7)
    setval('HX', 7); setval('HY', 7); settle()
    w.toggle_lever(lv[('PCHECK', 0)], False); t0 = w.time
    for _ in range(200):
        w.tick()
        if col(7): break
    print('PCHECK -> COL(7) latency', w.time - t0)
    w.toggle_lever(lv[('PCHECK', 0)], True); settle()
    setval('HX', 7); setval('HY', 6); settle()
    w.toggle_lever(lv[('PSET', 0)], False); t0 = w.time
    for _ in range(200):
        w.tick()
        if body(7, 6): break
    print('PSET -> (7,6) set latency', w.time - t0)
    w.toggle_lever(lv[('PSET', 0)], True); settle()
    # register change -> column select latency at the far row: change HX 7->6 and watch (6,6)'s column line at its last pixel
    x6 = P['HX'][6][0]
    setval('HX', 6); t0 = w.time
    for _ in range(200):
        w.tick()
        if w.get((x6, L0, 16 * 7 + 4)).power == 0: break
    print('HX lever -> column 6 selected at row 7 latency', w.time - t0)
    print('ok matrix (blocks %d)' % len(w.blocks))


if __name__ == '__main__':
    test()
