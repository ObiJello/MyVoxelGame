"""One dense pixel driven by levers on its eight lines."""
from build import Builder, Y
from redtick import TickWorld
from pixel import pixel, through_columns, through_rows, W, COL_X, OR_X, ROW_Z, L0, L2, L4


def build(x0=40, z0=40):
    b = Builder(TickWorld()); w = b.w
    info = pixel(b, x0, z0)
    # columns: levers north, lines through the pixel and one cell beyond
    for cx in COL_X:
        b.lever(x0 + cx, L0, z0 - 3, 'south'); b.wire(x0 + cx, L0, z0 - 2); b.wire(x0 + cx, L0, z0 - 1)
    through_columns(b, x0, z0, z0, z0 + W - 1)
    b.lamp(x0 + OR_X, L4, z0 + W)                      # collision OR line ends in a lamp
    # rows: levers west
    for rz in ROW_Z:
        b.lever(x0 - 3, L2, z0 + rz, 'east'); b.wire(x0 - 2, L2, z0 + rz); b.wire(x0 - 1, L2, z0 + rz)
    through_rows(b, x0, z0, x0, x0 + W - 1)
    assert not b.collisions, b.collisions
    return b, w, info


def test():
    x0, z0 = 40, 40
    b, w, info = build(x0, z0)
    L = {'HX': (x0 + COL_X[0], L0, z0 - 3), 'TX': (x0 + COL_X[1], L0, z0 - 3), 'FX': (x0 + COL_X[2], L0, z0 - 3),
         'HYS': (x0 - 3, L2, z0 + ROW_Z[0]), 'HYC': (x0 - 3, L2, z0 + ROW_Z[1]),
         'TYE': (x0 - 3, L2, z0 + ROW_Z[2]), 'FY': (x0 - 3, L2, z0 + ROW_Z[3])}
    for k in L:                                        # active-low lines idle HIGH
        w.toggle_lever(L[k], True)
    w.boot(); w.run_until_idle()
    body = lambda: w.lamp(info['body_lamp']); food = lambda: w.lamp(info['food_lamp'])
    dead = lambda: w.lamp((x0 + OR_X, L4, z0 + W))
    print('initial body', body(), 'food', food(), 'dead', dead())

    def pulse(*names, ticks=4):
        for n in names: w.toggle_lever(L[n], False)
        w.tick(ticks)
        for n in names: w.toggle_lever(L[n], True)
        w.run_until_idle()

    def level(name, on):
        w.toggle_lever(L[name], not on); w.run_until_idle()

    # reset first: TX selected (level), TYE pulse
    level('TX', True); pulse('TYE'); level('TX', False)
    assert not body(), 'reset'
    # food
    level('FX', True); level('FY', True)
    assert food() and not body(), 'food'
    level('FX', False); level('FY', False); assert not food()
    # head column selected; check pulse on an empty cell: no collision
    level('HX', True)
    w.toggle_lever(L['HYC'], False); w.tick(4); seen = dead()
    w.toggle_lever(L['HYC'], True); w.run_until_idle()
    assert not seen and not dead(), 'check on an empty cell must not report'
    # a set pulse with the head column unselected does nothing
    level('HX', False); pulse('HYS'); assert not body(), 'set needs the head column'
    level('HX', True)
    # set pulse
    pulse('HYS')
    assert body(), 'set'
    # check pulse on the set cell: collision reported for the pulse's duration
    w.toggle_lever(L['HYC'], False)
    hits = [w.lamp((x0 + OR_X, L4, z0 + W)) for _ in range(30) if not w.tick()]
    w.toggle_lever(L['HYC'], True); w.run_until_idle()
    assert any(hits), 'collision must reach the OR line'
    assert not dead()
    # check with the head column NOT selected: nothing
    level('HX', False)
    w.toggle_lever(L['HYC'], False)
    hits = [w.lamp((x0 + OR_X, L4, z0 + W)) for _ in range(30) if not w.tick()]
    w.toggle_lever(L['HYC'], True); w.run_until_idle()
    assert not any(hits), 'unselected column must not report'
    assert body(), 'set state must survive checks'
    # set pulse with the column unselected: nothing changes; erase needs TX + TYE
    pulse('TYE'); assert body(), 'erase needs the tail column'
    level('TX', True); pulse('TYE'); level('TX', False)
    assert not body(), 'erase'
    # lines idle: all lamps off, no stray power on the OR line
    assert not dead() and not food()
    print('ok pixel (blocks %d)' % len(w.blocks))


if __name__ == '__main__':
    test()
