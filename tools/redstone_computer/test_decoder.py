from build import Builder, Y
from redtick import TickWorld
from fabric import Fabric
from decoder import decoder3

def test():
    b = Builder(TickWorld()); w = b.w; f = Fabric(b, z0=10)
    q, nq = [], []
    for i in range(3):
        b.lever(i * 8, Y, 0, 'south'); q.append(f.new_lane(f'Q{i}', i * 8, 1, keep=True))
        # ~Q via a gate
    for i in range(3):
        nq.append(f.gate(f'NQ{i}', [q[i]], 30 + 6 * i, keep=True))
    d = decoder3(f, 'D', q, nq, 60)
    for l in d['SEL_L']:
        l.extend(l.z0 + 2); b.lamp(l.x, Y - 1, l.z0 + 2)
    f.build_lanes(); w.boot(); w.run_until_idle()
    for v in range(8):
        for i in range(3): w.toggle_lever((i * 8, Y, 0), (v >> i) & 1)
        w.run_until_idle()
        lit = [int(w.lamp((l.x, Y - 1, l.z0 + 2))) for l in d['SEL_L']]
        assert lit == [0 if k == v else 1 for k in range(8)], (v, lit)
    print('ok decoder3 (blocks %d)' % len(w.blocks))

test()
