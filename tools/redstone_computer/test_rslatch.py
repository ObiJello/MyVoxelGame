from build import Builder, Y
from redtick import TickWorld
from fabric import Fabric
from rslatch import rs_latch

def test():
    b = Builder(TickWorld()); w = b.w; f = Fabric(b, z0=10)
    b.lever(0, Y, 0, 'south'); s_in = f.new_lane('S', 0, 1)
    b.lever(6, Y, 0, 'south'); r_in = f.new_lane('R', 6, 1)
    # gates give lanes at chosen slots: S at 20, R at 24 (=20+4)
    s_l = f.gate('S_L', [s_in], 12); r_l = f.gate('R_L', [r_in], 16)
    S = f.gate('SET', [s_l], 24); R = f.gate('RESET', [r_l], 30)   # need slots xa, xa+6
    # place the latch at xa = 24, z below the gates' rows
    z = f.next_row_z + 6
    q, ql = rs_latch(f, 'D', S, R, 24, z)
    q.extend(z + 6); ql.extend(z - 6)
    b.lamp(q.x, Y - 1, z + 6); b.lamp(ql.x, Y - 1, z - 6)
    f.build_lanes(); w.boot(); w.run_until_idle()
    lamp_q = lambda: w.lamp((q.x, Y - 1, z + 6)); lamp_ql = lambda: w.lamp((ql.x, Y - 1, z - 6))
    w.toggle_lever((6, Y, 0), True); w.run_until_idle(); w.toggle_lever((6, Y, 0), False); w.run_until_idle()
    assert not lamp_q() and lamp_ql(), 'reset'
    w.toggle_lever((0, Y, 0), True); w.run_until_idle(); w.toggle_lever((0, Y, 0), False); w.run_until_idle()
    assert lamp_q() and not lamp_ql(), 'set'
    w.toggle_lever((6, Y, 0), True); w.run_until_idle(); w.toggle_lever((6, Y, 0), False); w.run_until_idle()
    assert not lamp_q() and lamp_ql(), 'reset again'
    print('ok rs_latch')

test()
