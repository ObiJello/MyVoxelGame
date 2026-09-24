from build import Builder, Y, dump
from redtick import TickWorld
from seq import pulse_gen, latch, tflop

def mk(): return Builder(TickWorld())

def hist_of(w, pos, n, field='power'):
    out = []
    for _ in range(n):
        w.tick(); v = getattr(w.get(pos), field); out.append(int(bool(v)))
    return out

def test_pulse_gen():
    b = mk(); w = b.w
    b.lever(0, Y, 5); b.wire(1, Y, 5)
    ox, oz = pulse_gen(b, 2, 5)
    w.boot(); w.run_until_idle()
    assert w.get((ox, Y, oz)).power == 0
    w.toggle_lever((0, Y, 5), True)
    h = hist_of(w, (ox, Y, oz), 20)
    # expect a 4-tick pulse; report its timing
    on = [i + 1 for i, v in enumerate(h) if v]
    assert len(on) == 4 and on == list(range(on[0], on[0] + 4)), h
    print('  pulse ticks', on)
    w.toggle_lever((0, Y, 5), False)
    h = hist_of(w, (ox, Y, oz), 20)
    assert sum(h) == 0, h                       # no pulse on the falling edge

def test_latch():
    b = mk(); w = b.w
    b.lever(0, Y, 0); b.wire(1, Y, 0)
    q = latch(b, 2, 0)                          # D at (2,0), WRITE at (3,4)
    b.lever(3, Y, 6); b.wire(3, Y, 5)
    w.boot(); w.run_until_idle()
    qp = (q[0], Y, q[1])
    w.toggle_lever((0, Y, 0), True); w.run_until_idle()
    assert w.get(qp).power == 0                 # held (write low = locked)
    w.toggle_lever((3, Y, 6), True); w.run_until_idle()
    assert w.get(qp).power > 0                  # write high = transparent
    w.toggle_lever((3, Y, 6), False); w.run_until_idle()
    w.toggle_lever((0, Y, 0), False); w.run_until_idle()
    assert w.get(qp).power > 0                  # held on

def test_tflop():
    b = mk(); w = b.w
    t = tflop(b, 2, 4)
    # pulse source: lever -> pulse_gen -> route to WRITE port from the south
    b.lever(0, Y, 20); b.wire(1, Y, 20)
    px, pz = pulse_gen(b, 2, 20)                # out at (9, 20)
    wx, wz = t['WRITE']
    b.line(px + 1, pz, wx, pz); b.line(wx, wz + 1, wx, pz - 1)
    w.boot(); w.run_until_idle()
    qp = (t['Q'][0], Y, t['Q'][1])
    q0 = int(w.get(qp).power > 0)
    seq = []
    for i in range(6):
        w.toggle_lever((0, Y, 20), True); w.run_until_idle()
        seq.append(int(w.get(qp).power > 0))
        w.toggle_lever((0, Y, 20), False); w.run_until_idle()
        assert int(w.get(qp).power > 0) == seq[-1]
    assert seq == [1 - q0, q0] * 3, (q0, seq)

def test_counter():
    from seq import counter_up
    b = mk(); w = b.w
    c = counter_up(b, 10, 10, 3)
    ix, iz = c['IN']
    b.button(ix - 4, Y, iz, 'south'); b.line(ix - 3, iz, ix - 1, iz)
    w.boot(); w.run_until_idle()
    def val():
        return sum((w.get((q[0], Y, q[1])).power > 0) << i for i, q in enumerate(c['Q']))
    start = val()
    for n in range(1, 10):
        w.press_button((ix - 4, Y, iz)); w.run_until_idle()
        assert val() == (start + n) % 8, (n, val(), start)
    print('  counter start', start)

for f in (test_pulse_gen, test_latch, test_tflop, test_counter):
    f(); print('ok', f.__name__)
