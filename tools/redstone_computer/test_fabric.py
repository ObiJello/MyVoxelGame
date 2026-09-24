import itertools
from build import Builder, Y, dump
from redtick import TickWorld
from fabric import Fabric

def test_and_chain():
    b = Builder(TickWorld()); f = Fabric(b, z0=4); w = b.w
    # three input lanes from levers
    ins = []
    for i, x in enumerate((0, 3, 6)):
        b.lever(x, Y, 0, 'south')
        ins.append(f.new_lane(f'in{i}', x, 1, keep=True))
    # g1 = NOR(in0, in1); g2 = NOR(g1)  (= in0 | in1); g3 = NOR(g2, in2) = ~in0 & ~in1 & ~in2
    g1 = f.gate('g1', ['in0', 'in1'], 15)
    g2 = f.gate('g2', ['g1'], 24)
    g3 = f.gate('g3', ['g2', 'in2'], 33)
    g3.extend(g3.z0 + 3); f.build_lanes()
    b.lamp(33, Y - 1, g3.z1 + 1); b.wire(33, Y, g3.z1 + 1)
    w.boot(); w.run_until_idle()
    for bits in itertools.product([0, 1], repeat=3):
        for x, v in zip((0, 3, 6), bits): w.toggle_lever((x, Y, 0), v)
        w.run_until_idle()
        assert w.lamp((33, Y - 1, g3.z1 + 1)) == (not any(bits)), (bits, w.lamp((33, Y - 1, g3.z1 + 1)))
    print('ok fabric and-chain')

test_and_chain()
