"""3-bit synchronous up/down counter on the fabric.

    updown3(f, name, x0, z0, step_l, up_l)

Three toggle latches (seq.tflop) at z0 with pitch 16 from x0; their Q and ~Q
become lanes running south. Gates east of them form
    T0 = STEP,  T1 = STEP & (UP ? Q0 : ~Q0),  T2 = STEP & (UP ? Q0&Q1 : ~Q0&~Q1)
and each T pulse comes back west on the upper layer to a floor lane under the
stage's WRITE port. `step_l` and `up_l` are active-low lanes west of x0.
Returns dict with lanes 'Q0'..'Q2', 'NQ0'..'NQ2' and the gate slot after use.
"""
from build import Y
from seq import tflop
from cells import run_wire

PITCH = 12   # x0 = 2 (mod 3) puts NQ (x0-2), W (x0+4) and Q (x0+7) on the slot grid; 12 keeps them there per stage


def updown3_place(f, name, x0, z0):
    """Three toggle flip-flops at pitch 16 from x0; Q/~Q become lanes."""
    b = f.b
    f.reserve_floor(x0 - 2, x0 + PITCH * 2 + 9, z0 - 4, z0 + 9)
    q, nq, write = [], [], []
    for i in range(3):
        sx = x0 + PITCH * i
        t = tflop(b, sx, z0)
        b.lamp(t['Q'][0], Y - 1, t['Q'][1])
        q.append(f.new_lane(f'{name}Q{i}', t['Q'][0], t['Q'][1] + 1, loose=True))
        nq.append(f.new_lane(f'{name}NQ{i}', t['NOTQ'][0], t['NOTQ'][1] + 1, loose=True))
        # the WRITE column: a north-flowing lane driven later by feedback
        wx, wz = t['WRITE']
        wl = f.new_lane(f'{name}.W{i}', wx, 100000, flow='north', loose=True)
        wl.extend(wz + 1)
        write.append(wl)
    return {'Q': q, 'NQ': nq, 'write': write, 'x0': x0, 'z0': z0}


def updown3_logic(f, name, c, step_l, up_l, t_slot=None, reset=None):
    """The toggle gates and their pulses back to the WRITE ports.

    The carry gates are levels and may sit anywhere; the three T gates are
    on the step's critical path, so `t_slot(inputs)` (a slot finder near the
    counter, e.g. Machine3._slot_for with prefer just east of it) keeps
    their rows short: a step then reaches the storage in ~60-80 ticks
    instead of the ~200 a T gate placed next to a far input costs."""
    b = f.b
    q, nq, write = c['Q'], c['NQ'], c['write']
    step_l.keep = True; up_l.keep = True
    wcols = tuple(wl.x for wl in write)          # the pulses must feed back to these
    def G(nm, ins, **kw):
        return f.gate(f"{name}.{nm}", ins, avoid=wcols, **kw)
    def T(nm, ins, **kw):
        if t_slot is not None:
            kw['x'] = t_slot(ins)
        return G(nm, ins, **kw)
    up = G('UP', [up_l], keep=True)
    t0 = T('T0', [step_l])
    cu1 = G('cu1', [up_l, nq[0]]); cd1 = G('cd1', [up, q[0]])
    c1l = G('c1L', [cu1, cd1], done=True)
    t1 = T('T1', [step_l, c1l], close=[c1l])
    a = G('a', [nq[0], nq[1]]); al = G('aL', [a], done=True)
    cu2 = G('cu2', [up_l, al], close=[al])
    bb = G('b', [q[0], q[1]]); bl = G('bL', [bb], done=True)
    cd2 = G('cd2', [up, bl], close=[bl])
    c2l = G('c2L', [cu2, cd2], done=True)
    t2 = T('T2', [step_l, c2l], close=[c2l])
    for i, t in enumerate((t0, t1, t2)):
        wl = write[i]
        if reset is not None:
            reset(i, t)                     # a gate JOINED onto the T lane (restart: conditional toggle), before it closes
        fx, fz = f.feedback(t, wl.x)
        wl.taps.add(fz); wl.z0 = fz                        # the lane really starts at the landing
        f.close(t)
    return c


def updown3(f, name, x0, z0, step_l, up_l, gate_x=None):
    c = updown3_place(f, name, x0, z0)
    return updown3_logic(f, name, c, step_l, up_l)
