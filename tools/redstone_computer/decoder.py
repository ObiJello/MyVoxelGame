"""3-to-8 decoder on the fabric: SEL_L[v] is LOW exactly when the 3-bit value
equals v. SEL[v] = NOR(the three lines that must be low for v), SEL_L[v] =
NOR(SEL[v]). Two gates per output, 16 slots of 6 cells."""


def decoder3(f, name, q, nq, gate_x):
    sel_l = []
    gx = gate_x
    for v in range(8):
        must_low = [nq[i] if (v >> i) & 1 else q[i] for i in range(3)]
        sel = f.gate(f'{name}.S{v}', must_low, gx)
        if gx is not None: gx += 6
        sel_l.append(f.gate(f'{name}.SL{v}', [sel], gx, keep=True))
        if gx is not None: gx += 6
    return {'SEL_L': sel_l, 'gate_x': gx}


def decoder4(f, name, q, nq, gate_x, values=range(1, 16), keep_sel=()):
    """4-to-16 decoder (subset `values`). Four gates per output:
    g1 = NOR(m0,m1,m2); g1L = NOR(g1); SEL = NOR(g1L, m3); SEL_L = NOR(SEL)."""
    sel_l = {}
    gx = gate_x
    for v in values:
        m = [nq[i] if (v >> i) & 1 else q[i] for i in range(4)]
        g1 = f.gate(f'{name}.g1_{v}', m[:3], gx)
        if gx is not None: gx += 6
        g1l = f.gate(f'{name}.g1L_{v}', [g1], gx, done=True)
        if gx is not None: gx += 6
        sel = f.gate(f'{name}.S{v}', [g1l, m[3]], gx, close=[g1l], keep=(v in keep_sel))
        if gx is not None: gx += 6
        sel_l[v] = f.gate(f'{name}.SL{v}', [sel], gx, keep=True)
        if gx is not None: gx += 6
    return {'SEL_L': sel_l, 'gate_x': gx}
