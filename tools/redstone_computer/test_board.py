"""8x8 board driven by levers on every line."""
import sys
from build import Builder
from redtick import TickWorld
from board import board, N, SIZE
from pixel import L0, L2, L4


def build(x0=100, z0=100):
    b = Builder(TickWorld()); w = b.w
    P = board(b, x0, z0)
    lv = {}
    for k in ('HX', 'TX', 'FX'):
        for i, (x, y, z) in enumerate(P[k]):
            b.lever(x, y, z - 2, 'south'); b.wire(x, y, z - 1)
            lv[(k, i)] = (x, y, z - 2)
    for k in ('HYS', 'HYC', 'TYE', 'FY'):
        for j, (x, y, z) in enumerate(P[k]):
            b.lever(x - 2, y, z, 'east'); b.wire(x - 1, y, z)
            lv[(k, j)] = (x - 2, y, z)
    for i, (x, y, z) in enumerate(P['COL']):
        b.lamp(x, y, z + 1)
    assert not b.collisions, b.collisions[:10]
    return b, w, P, lv


def test():
    x0, z0 = 100, 100
    b, w, P, lv = build(x0, z0)
    for k in lv:
        w.toggle_lever(lv[k], True)
    w.boot(); w.run_until_idle()
    body = lambda i, j: w.lamp(P['body'][(i, j)])
    food = lambda i, j: w.lamp(P['food'][(i, j)])
    col = lambda i: w.lamp((P['COL'][i][0], L4, P['COL'][i][2] + 1))
    lit = lambda: {(i, j) for i in range(N) for j in range(N) if body(i, j)}
    print('blocks', len(w.blocks), 'initial lit', sorted(lit()), 'food', [(i, j) for i in range(N) for j in range(N) if food(i, j)])

    def level(k, n, on):
        w.toggle_lever(lv[(k, n)], not on)

    def pulse(k, n, ticks=4):
        w.toggle_lever(lv[(k, n)], False); w.tick(ticks); w.toggle_lever(lv[(k, n)], True)

    def settle():
        w.run_until_idle(limit=2000)

    # erase everything: for each row, select all TX columns and pulse TYE
    for i in range(N): level('TX', i, True)
    for j in range(N): pulse('TYE', j); settle()
    for i in range(N): level('TX', i, False)
    settle()
    assert not lit(), sorted(lit())
    # set (2,3), (7,7), (0,0), (5,3): head column HX=i, pulse HYS=j
    for (i, j) in ((2, 3), (7, 7), (0, 0), (5, 3)):
        level('HX', i, True); settle(); pulse('HYS', j); settle(); level('HX', i, False); settle()
    assert lit() == {(2, 3), (7, 7), (0, 0), (5, 3)}, sorted(lit())
    # check on (2,3): collision on column 2 only; on (2,4): none
    def check(i, j):
        level('HX', i, True); settle()
        w.toggle_lever(lv[('HYC', j)], False)
        hits = set()
        for _ in range(60):
            w.tick()
            hits |= {c for c in range(N) if col(c)}
        w.toggle_lever(lv[('HYC', j)], True); settle(); level('HX', i, False); settle()
        return hits
    assert check(2, 3) == {2}, check(2, 3)
    assert check(2, 4) == set()
    assert check(7, 7) == {7}
    assert check(6, 7) == set()
    assert check(0, 0) == {0}
    assert check(5, 3) == {5}
    assert check(3, 3) == set()
    assert lit() == {(2, 3), (7, 7), (0, 0), (5, 3)}, 'checks must not change the board'
    # erase (2,3) and (7,7)
    for (i, j) in ((2, 3), (7, 7)):
        level('TX', i, True); settle(); pulse('TYE', j); settle(); level('TX', i, False); settle()
    assert lit() == {(0, 0), (5, 3)}, sorted(lit())
    # food at (6, 1) and (0, 7)
    level('FX', 6, True); level('FY', 1, True); settle()
    assert [(i, j) for i in range(N) for j in range(N) if food(i, j)] == [(6, 1)]
    level('FX', 6, False); level('FY', 1, False); level('FX', 0, True); level('FY', 7, True); settle()
    assert [(i, j) for i in range(N) for j in range(N) if food(i, j)] == [(0, 7)]
    level('FX', 0, False); level('FY', 7, False); settle()
    assert not any(food(i, j) for i in range(N) for j in range(N))
    # timing: how long from the HYS lever to the far pixel's set, and from HYC to the OR lamp
    level('HX', 7, True); settle()
    w.toggle_lever(lv[('HYS', 7)], False); t0 = w.time
    for _ in range(100):
        w.tick()
        if body(7, 7): break
    print('set latency to (7,7):', w.time - t0, 'ticks')
    w.toggle_lever(lv[('HYS', 7)], True); settle()
    w.toggle_lever(lv[('HYC', 7)], False); t0 = w.time
    for _ in range(100):
        w.tick()
        if col(7): break
    print('check latency (7,7) -> OR lamp:', w.time - t0, 'ticks')
    w.toggle_lever(lv[('HYC', 7)], True); settle(); level('HX', 7, False); settle()
    print('ok board (blocks %d)' % len(w.blocks))


if __name__ == '__main__':
    test()
