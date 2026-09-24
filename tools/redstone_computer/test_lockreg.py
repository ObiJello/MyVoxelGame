from build import Builder, Y
from redtick import TickWorld
from fabric import Fabric
from lockreg import reg_bit
from seq import pulse_gen
from cells import run_wire

def test():
    b = Builder(TickWorld()); w = b.w; f = Fabric(b, z0=10)
    b.lever(0, Y, 0, 'south'); d_in = f.new_lane('D', 0, 1)
    b.lever(-30, Y, 0, 'south'); b.wire(-29, Y, 0)
    px, pz = pulse_gen(b, -28, 0); run_wire(b, [(px, pz), (px, 8)])
    wp = f.new_lane('WP', px, 9)
    d_l = f.gate('D_L', [d_in], 8); D = f.gate('DD', [d_l], 20)
    z = f.next_row_z + 6                      # register row, south of D's rows
    w_l = f.gate('W_L', [wp], 14)             # rows allocated now (south of z? no: z was taken before)
    # the W gate must be SOUTH of the register so its north-flowing lane reaches the stub
    W = f.gate('WW', [w_l], 21, out_flow='north', loose=True, z_min=z + 10)
    q = reg_bit(f, 'Q', D, W, 20, z)
    q.extend(q.z0 + 3); b.lamp(q.x, Y - 1, q.z0 + 3)
    f.build_lanes(); w.boot(); w.run_until_idle()
    lamp = lambda: w.lamp((q.x, Y - 1, q.z0 + 3))
    def pulse():
        w.toggle_lever((-30, Y, 0), True); w.run_until_idle(); w.toggle_lever((-30, Y, 0), False); w.run_until_idle()
    w.toggle_lever((0, Y, 0), True); w.run_until_idle()
    assert not lamp(), 'held low before write'
    pulse(); assert lamp(), 'captured 1'
    w.toggle_lever((0, Y, 0), False); w.run_until_idle(); assert lamp(), 'held high'
    pulse(); assert not lamp(), 'captured 0'
    print('ok reg_bit')

test()
