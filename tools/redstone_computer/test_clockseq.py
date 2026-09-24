from build import Builder, Y
from redtick import TickWorld
from fabric import Fabric
from clockseq import oscillator, sequencer

def test():
    b = Builder(TickWorld()); w = b.w; f = Fabric(b, z0=40)
    cx, cz = oscillator(b, 0, 0, 10, pause=False)   # period 2*(2+80+2*1) = 168
    clk = f.new_lane('CLK', cx, cz, 'south')
    b.lever(-10, Y, 0, 'south'); dead = f.new_lane('DEAD', -10, 1)   # lever on = dead
    clk_l = f.gate('CLK_L', [clk], 30)
    g = f.gate('G', [clk_l, dead], 36)         # CLK & ~DEAD
    ps = sequencer(f, g, 36, f.next_row_z + 10)
    for l in ps: l.extend(l.z0 + 2)
    f.build_lanes(); w.boot()
    w.tick(400)
    # record rising edges of each phase over one period
    hist = {l.name: [] for l in ps}
    for t in range(400):
        w.tick()
        for l in ps:
            hist[l.name].append(w.get((l.x, Y, l.z0 + 2)).power > 0)
    edges = {}
    for n, h in hist.items():
        edges[n] = [i for i in range(1, len(h)) if h[i] and not h[i - 1]]
        widths = []
        i = 0
        while i < len(h):
            if h[i]:
                j = i
                while j < len(h) and h[j]: j += 1
                widths.append(j - i); i = j
            else: i += 1
        print(f'  {n}: edges {edges[n][:3]} widths {widths[:3]}')
    p1 = edges['P1']
    assert len(p1) >= 2 and p1[1] - p1[0] == 168, p1
    for k in range(1, 7):
        d = edges[f'P{k + 1}'][0] - edges['P1'][0]
        assert d == 16 * k, (k, d)
    # dead stops the clock pulses
    w.toggle_lever((-10, Y, 0), True); w.tick(200)
    quiet = True
    for t in range(200):
        w.tick()
        if w.get((ps[0].x, Y, ps[0].z0 + 2)).power: quiet = False
    assert quiet
    print('ok clock + sequencer')

test()
