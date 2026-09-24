from build import Builder, Y
from redtick import TickWorld
from fabric import Fabric
from fifo import Fifo
from decoder import decoder4
from seq import pulse_gen
from cells import run_wire

def test(stages=6):
    b = Builder(TickWorld()); w = b.w; f = Fabric(b, z0=20)
    # data lever -> lane D at slot x0-3 = 37
    b.lever(37, Y, 0, 'south'); D = f.new_lane('D', 37, 1)
    fifo = Fifo(f, 'F', D, stages, 40, 30)
    # shift pulse: lever -> pulse_gen -> lane S (slot 0) -> S_L gate (slot 10)
    b.lever(-30, Y, 0, 'south'); b.wire(-29, Y, 0)
    px, pz = pulse_gen(b, -28, 0); run_wire(b, [(px, pz), (px, 8)])
    S = f.new_lane('S', px, 9); S_L = f.gate('S_L', [S], 10, keep=True)
    # L value levers -> Q lanes (slots 14,17,20,23) and NQ gates
    lq, lnq = [], []
    for i in range(4):
        b.lever(-80 + 3 * i, Y, 0, 'south'); lq.append(f.new_lane(f'L{i}', -80 + 3 * i, 1, keep=True))
    gx = -60
    for i in range(4):
        lnq.append(f.gate(f'LN{i}', [lq[i]], gx, keep=True)); gx += 6   # -60..-42
    # rows so far are north of the FIFO? The fifo occupies z 30..30+8*stages; the
    # write gates must be south of it: push the row allocator past it.
    for k in range(stages):
        fifo.drive_write(k, lambda x, join: f.gate(f'W{k}', [S_L], x, out_flow='north', loose=True,
                                                   z_min=30 + 8 * stages + 10, join=join))
    # decoder for L (values 1..stages) east of the FIFO, then the mux
    gx = 40 + 6 * stages + 10
    dec = decoder4(f, 'LD', lq, lnq, gx, values=range(1, stages + 1))
    gx = dec['gate_x']
    qls = []
    for k in range(stages):
        qls.append(f.gate(f'QL{k}', [fifo.q[k]], gx)); gx += 6
    outx = gx
    OUT = f.gate('OUT', [dec['SEL_L'][1], qls[0]], outx)
    for k in range(1, stages):
        f.gate(f'OUT{k}', [dec['SEL_L'][k + 1], qls[k]], outx, join=OUT)
    OUT.extend(OUT.z1 + 3); b.lamp(OUT.x, Y - 1, OUT.z1)
    f.build_lanes(); w.boot(); w.run_until_idle()
    def shift(v):
        w.toggle_lever((37, Y, 0), v); w.run_until_idle()
        w.toggle_lever((-30, Y, 0), True); w.run_until_idle(); w.toggle_lever((-30, Y, 0), False); w.run_until_idle()
    def setL(v):
        for i in range(4): w.toggle_lever((-80 + 3 * i, Y, 0), (v >> i) & 1)
        w.run_until_idle()
    seq = [1, 0, 1, 1, 0, 0, 1, 0]
    hist = []
    for v in seq[:stages]:
        shift(v); hist.insert(0, v)              # hist[k] = value k shifts ago
    for L in range(1, stages + 1):
        setL(L)
        got = int(w.lamp((OUT.x, Y - 1, OUT.z1)))
        assert got == hist[L - 1], (L, got, hist)
    # one more shift and re-check
    shift(1); hist.insert(0, 1)
    for L in range(1, stages + 1):
        setL(L); got = int(w.lamp((OUT.x, Y - 1, OUT.z1)))
        assert got == hist[L - 1], (L, got, hist)
    print(f'ok fifo x{stages} + decoder4 + mux (blocks {len(w.blocks)})')

test()
