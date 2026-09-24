"""Tail mux matrix driven by levers: 8 length lines, 2 x 12 stored-bit lines."""
from build import Builder
from redtick import TickWorld
from pixel import L0
from tailmux import tailmux, tap_cells, STAGES, bits4


def build():
    b = Builder(TickWorld()); w = b.w
    l_lines = {}
    lv = {}
    x = 0
    for n in ('Q0', 'NQ0', 'Q1', 'NQ1', 'Q2', 'NQ2', 'Q3', 'NQ3'):
        l_lines[n] = x; x += 6
    x0 = 66
    fifo_q = [[x0 + 3 + 6 * s for s in range(STAGES)], [x0 + 90 + 3 + 6 * s for s in range(STAGES)]]
    td_x = [fifo_q[0][-1] + 9, fifo_q[1][-1] + 9]
    z_top = 20
    info = tailmux(b, l_lines, fifo_q, z_top, td_x)
    taps = tap_cells(l_lines, fifo_q, z_top)
    # lines from levers at z=10 down to z_line_end, repeaters every 12 off the tap cells
    for xl in list(l_lines.values()) + fifo_q[0] + fifo_q[1]:
        b.lever(xl, L0, 10, 'south')
        lv[xl] = (xl, L0, 10)
        since = 0
        for z in range(11, info['z_line_end'] + 1):
            since += 1
            if since >= 12 and z not in taps.get(xl, ()) and z + 1 not in taps.get(xl, ()):
                b.repeater(xl, L0, z, 'north'); since = 0
            else:
                b.wire(xl, L0, z)
    # TD lanes: a few cells south then a lamp
    for tx in td_x:
        for z in range(info['z_bus'] + 1, info['z_bus'] + 4):
            b.wire(tx, L0, z)
        b.lamp(tx, L0, info['z_bus'] + 4)
    assert not b.collisions, b.collisions[:10]
    return b, w, l_lines, fifo_q, td_x, lv, info


def test():
    b, w, l_lines, fifo_q, td_x, lv, info = build()
    for k in lv: w.toggle_lever(lv[k], True)
    w.boot(); w.run_until_idle()
    td = lambda bit: w.lamp((td_x[bit], L0, info['z_bus'] + 4))

    def set_L(v):
        for bt in range(4):
            on = (v >> bt) & 1
            w.toggle_lever(lv[l_lines['Q%d' % bt]], bool(on))
            w.toggle_lever(lv[l_lines['NQ%d' % bt]], not on)

    def set_fifo(bit, values):
        """values[s] = stored direction bit of stage s (the line carries NOT bit)."""
        for s in range(STAGES):
            w.toggle_lever(lv[fifo_q[bit][s]], not values[s])

    import random
    rng = random.Random(3)
    print('blocks', len(w.blocks))
    for trial in range(40):
        v0 = [rng.randint(0, 1) for _ in range(STAGES)]
        v1 = [rng.randint(0, 1) for _ in range(STAGES)]
        L = rng.randint(1, 12)
        set_fifo(0, v0); set_fifo(1, v1); set_L(L)
        w.run_until_idle(limit=5000)
        want = (v0[L - 1], v1[L - 1])
        got = (int(td(0)), int(td(1)))
        assert got == want, (trial, L, v0, v1, got, want)
    # latency: L change -> TD
    set_L(3); w.run_until_idle(limit=5000)
    set_fifo(0, [1] * STAGES); set_fifo(1, [0] * STAGES); w.run_until_idle(limit=5000)
    assert td(0) and not td(1)
    set_L(9); t0 = w.time
    w.run_until_idle(limit=5000)
    print('settle after L change:', w.time - t0, 'ticks')
    print('ok tailmux (blocks %d)' % len(w.blocks))


if __name__ == '__main__':
    test()
