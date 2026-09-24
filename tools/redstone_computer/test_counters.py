from build import Builder, Y, dump
from redtick import TickWorld
from fabric import Fabric
from counters import updown3
from seq import pulse_gen
from cells import run_wire

def test_updown():
    b = Builder(TickWorld()); w = b.w
    # STEP: lever -> pulse_gen -> floor wire -> gate (NOT) -> STEP_L lane. Simpler:
    # feed the pulse into a lane and let a fabric gate invert it.
    f = Fabric(b, z0=30)
    b.lever(-30, Y, 0, 'south'); b.wire(-29, Y, 0)
    px, pz = pulse_gen(b, -28, 0)              # out (-21, 0)
    run_wire(b, [(px, pz), (px, 20)])          # down to z=20 then it's lane 'P' at x=-21
    P = f.new_lane('P', px, 21)
    b.lever(-16, Y, 0, 'south')
    UPL = f.new_lane('UP_L', -16, 1, keep=True)   # lever ON = UP_L high = count DOWN
    STEPL = f.gate('STEP_L', ['P'], -10, keep=True)
    c = updown3(f, 'X', 0, 8, STEPL, UPL, gate_x=60)
    f.build_lanes()
    w.boot(); w.run_until_idle()
    qs = c['Q']
    def val():
        return sum((w.get((l.x, Y, l.z0)).power > 0) << i for i, l in enumerate(qs))
    v0 = val(); print('  start', v0)
    seq = []
    for n in range(1, 10):
        w.toggle_lever((-30, Y, 0), True); w.run_until_idle()
        w.toggle_lever((-30, Y, 0), False); w.run_until_idle()
        seq.append(val())
    assert seq == [(v0 + n) % 8 for n in range(1, 10)], (v0, seq)
    w.toggle_lever((-16, Y, 0), True); w.run_until_idle()    # count down
    v1 = val(); seq = []
    for n in range(1, 10):
        w.toggle_lever((-30, Y, 0), True); w.run_until_idle()
        w.toggle_lever((-30, Y, 0), False); w.run_until_idle()
        seq.append(val())
    assert seq == [(v1 - n) % 8 for n in range(1, 10)], (v1, seq)
    print('ok updown3 (blocks %d)' % len(w.blocks))
    return b

if __name__ == '__main__':
    test_updown()
