"""Snake v2: the dense board (board.py) with repeater-tap select matrices
(matrix.py), and the control logic on the fabric north-west of it.

Board at (0, 0)..(127, 127). North of it the X matrix: 18 select lines at
level 2 (z = -8 - 4k) tapped into the 24 board columns. West of it the Y
matrix: fabric lanes at x = -72 - 6k (three pulse lines first, then the HY,
TY and FY select lines) tapped into the 32 board rows. The collision OR
lines leave the board south, are collected on a bus at z = 129 and come
back north on the NCOL lane at x = -12.

Phases (4-tick pulses from the sequencer, spacing SPACING):
  A  head step (STEPX/STEPY), FIFO shift, wall check
  B  tail erase pulse + tail step unless GROW (GROW = head == food, combinational)
  C  collision check pulse (HYC rows)
  D  head set pulse (HYS rows)
  E  L++ and food <- RNG if GROW; DIR <- REQ
"""
import os
from build import Builder, Y
from redtick import TickWorld
from fabric import Fabric
from pcb import Pcb
from counters import updown3_place, updown3_logic, PITCH
from seq import counter_up
from rslatch import rs_latch
from lockreg import reg_bit
from fifo import Fifo
from decoder import decoder4
from clockseq import oscillator, sequencer, sequencer_width
from board import board, N
from matrix import xmatrix, ymatrix, xline, ytap, yrow, ROW_SIDE
from pixel import W as PW, L0, L2, L4, ROW_Z

BX, BZ = 0, 0                    # board origin
ZB = -1400                        # fabric row base (rows grow towards the board)
FAR = 10 ** 5
STAGES = 12
SPACING = (448, 128, 16, 40)     # A->B, B->C, C->D, D->E (multiples of 8): measured on the unoptimised layout
BITS = ['Q0', 'NQ0', 'Q1', 'NQ1', 'Q2', 'NQ2']
NCOL_X = -12
XLANE_X0 = -15                   # X line source lanes at -15, -18, ..., -66
YLINE_X0 = -72                   # pulse lines at -72, -78, -84; HY -90..; TY -126..; FY -162..


def bits_for(v):
    return ['NQ%d' % b if (v >> b) & 1 else 'Q%d' % b for b in range(3)]


class Machine2:
    def __init__(self, clock_n=85):
        self.b = Builder(TickWorld()); self.w = self.b.w
        self.f = Fabric(self.b, z0=ZB)
        self.f.x_min = -1080
        self.f.x_max = -200
        self.clock_n = clock_n
        self.regs = {}
        self.build()

    def G(self, name, inputs, **kw):
        return self.f.gate(name, inputs, **kw)

    # ── helpers (as in snake_machine) ────────────────────────────────────
    def _strip(self, width):
        x0 = self.strip_x
        self.strip_x += width + 6
        return x0

    def _reg(self, name, d_lane, w_lane, z, x=None):
        f = self.f
        if x is None:
            def ok(xc):
                return not any(f.slot_blocked(s, ZB) or not f.slot_free_below(s, ZB)
                               for s in (xc, xc + 1, xc + 3))
            xe = ((max(d_lane.x, w_lane.x) + 8) // 3) * 3
            xw = ((min(d_lane.x, w_lane.x) - 12) // 3) * 3
            for _ in range(800):
                if ok(xe) and xe <= f.x_max: x = xe; break
                if ok(xw): x = xw; break
                xe += 3; xw -= 3
            assert x is not None, name
        keep_out = (x, x + 1, x + 3)
        dl = self.G(name + '.dl', [d_lane], avoid=keep_out)
        D = self.G(name + '.d', [dl], x=x)
        wl = self.G(name + '.wl', [w_lane], avoid=keep_out)
        Wl = self.G(name + '.w', [wl], x=x + 1, out_flow='north', loose=True, z_min=z + 10)
        self.regs[name] = (x + 1, Y, z)
        return reg_bit(f, name, D, Wl, x, z, lamp=True)

    def _rs(self, name, s_src, r_src, ql=False):
        f = self.f
        span = range(-3, 10)
        def ok(xc):
            return xc % 3 == 0 and xc + 9 <= f.x_max and not any(
                f.slot_blocked(xc + o, ZB) or not f.slot_free_below(xc + o, ZB) for o in span)
        xe = ((s_src.x + 8) // 3) * 3
        xw = ((s_src.x - 20) // 3) * 3
        xa = None
        for _ in range(800):
            if ok(xe): xa = xe; break
            if ok(xw): xa = xw; break
            xe += 3; xw -= 3
        assert xa is not None, name
        keep_out = tuple(xa + o for o in span)
        gs = self.G(name + '.S', [s_src], x=xa, avoid=keep_out)
        gr = None
        if r_src is not None:
            gr = self.G(name + '.R', [r_src], x=xa + 6, loose=True, avoid=keep_out)
        z = max(gs.z1, gr.z1 if gr else -10**9) + 6
        f.reserve_floor(xa - 3, xa + 9, z - 1, z + 3)
        return rs_latch(f, name, gs, gr, xa, z, ql=ql)

    def _place_updown(self, name):
        x0 = self._strip(38) + 2
        x0 += (2 - x0) % 3
        c = updown3_place(self.f, name, x0, ZB - 40)
        for i in range(3):
            self.regs[f'{name}{i}'] = (x0 + PITCH * i + 4, Y, ZB - 40)
        return c

    def _place_counter(self, name, bits):
        f, b = self.f, self.b
        width = 18 * bits + 4
        x0 = self._strip(width + 8) + 6
        x0 += (2 - x0) % 3
        z0 = ZB - 60
        f.reserve_floor(x0 - 6, x0 + width, z0 - 6, z0 + 12)
        c = counter_up(b, x0, z0, bits, lamps=True)
        ix, iz = c['IN']
        b.wire(x0 - 4, Y, iz)
        inl = f.new_lane(f'{name}.IN', x0 - 5, FAR, flow='north', loose=True)
        inl.extend(iz)
        q = []
        for i, (qx, qz) in enumerate(c['Q']):
            q.append(f.new_lane(f'{name}{i}', qx, qz + 1, loose=True))
            self.regs[f'{name}{i}'] = (x0 + 18 * i + 4, Y, z0)
        return q, inl

    def _place_fifo(self, name, stages):
        f = self.f
        FW = 3 + 6 * stages + 6
        x0 = self._strip(FW + 3) + 3
        x0 -= x0 % 3
        fz = ZB - 250
        f.reserve_floor(x0 - 4, x0 + FW, fz - 2, fz + 8 * stages + 6)
        dn = f.new_lane(f'{name}.D', x0 - 3, FAR, flow='north', loose=True)
        fifo = Fifo(f, name, dn, stages, x0, fz)
        return fifo, dn

    def _drive_placeholder(self, lane, src):
        fx, fz = self.f.feedback(src, lane.x)
        lane.taps.add(fz)
        lane.z0 = fz

    def _onehot(self, name, d0, d1):
        d0.keep = True; d1.keep = True
        nd0 = self.G(f'{name}.n0', [d0], keep=True); nd1 = self.G(f'{name}.n1', [d1], keep=True)
        Nn = self.G(f'{name}.N', [d0, d1]);   NL = self.G(f'{name}.N_L', [Nn], keep=True)
        E = self.G(f'{name}.E', [nd0, d1]);  EL = self.G(f'{name}.E_L', [E], keep=True)
        S = self.G(f'{name}.S', [d0, nd1]);  SL = self.G(f'{name}.S_L', [S], keep=True)
        Wg = self.G(f'{name}.W', [nd0, nd1]); WL = self.G(f'{name}.W_L', [Wg], keep=True)
        return {'N_L': NL, 'E_L': EL, 'S_L': SL, 'W_L': WL, 'n0': nd0, 'n1': nd1}

    def _free_slot(self, x, step=3, inputs=()):
        """A free slot at or east of x (west with step < 0), inside the
        fabric's x range and more than 4 cells from every input lane."""
        f = self.f
        x = ((x + 2) // 3) * 3
        for _ in range(400):
            if (f.x_min <= x <= f.x_max and not f.by_slot.get(x) and not f.slot_blocked(x)
                    and all(abs(x - l.x) > 4 for l in inputs)):
                return x
            x += step
        raise AssertionError(('no free slot near', x))

    def _slot_for(self, inputs):
        """A free slot for a gate reading `inputs`: east of them if the
        fabric has room there, else west of them."""
        hi = max(l.x for l in inputs) + 5
        try:
            return self._free_slot(hi, inputs=inputs)
        except AssertionError:
            return self._free_slot_west(min(l.x for l in inputs) - 5, inputs=inputs)

    def _free_slot_west(self, x, inputs=()):
        """A free slot at or west of x with the same rules, but any x."""
        f = self.f
        x = (x // 3) * 3
        for _ in range(400):
            if (not f.by_slot.get(x) and not f.slot_blocked(x) and all(abs(x - l.x) > 4 for l in inputs)):
                return x
            x -= 3
        raise AssertionError(('no free slot west of', x))

    # ── build ───────────────────────────────────────────────────────────
    def build(self):
        b, f, w = self.b, self.f, self.w
        # the board and the X matrix region are off limits to the fabric
        f.reserve_floor(BX - 8, BX + 8 * PW + 2, -80, BZ + 8 * PW + 3)
        self.ports = board(b, BX, BZ)

        # ---------------- clock + sequencer ----------------
        self.strip_x = -800
        ox = -1000
        ox += (-(ox + self.clock_n + 4)) % 3                # CLK tap (ox + n + 4) on a slot
        cx, cz = oscillator(b, ox, ZB - 60, self.clock_n)
        assert cx % 3 == 0, cx
        f.reserve_floor(ox - 3, cx + 2, ZB - 64, ZB - 57)
        self.pause = [(ox, Y, ZB - 63)]
        clk = f.new_lane('CLK', cx, cz, loose=True)
        clk_l = self.G('CLK_L', [clk])
        # placeholder columns driven by feedback later: west of x_min, where the
        # automatic placer never goes (an unstarted placeholder looks free)
        self.deadfb = f.new_lane('DEADFB', -1110, FAR, flow='north', loose=True)
        g = self.G('G', [clk_l, self.deadfb], min_x=-990, side='east')
        seq_z = g.z0 + 8
        f.reserve_floor(g.x - 1, g.x + sequencer_width(5, SPACING) + 2, seq_z - 1, seq_z + 8)
        self.P = sequencer(f, g, g.x, seq_z, phases=5, spacing=list(SPACING))
        P = self.P
        PL = [self.G(f'P{k}_L', [p], keep=True) for k, p in zip('ABCDE', P)]
        self.PL = dict(zip('ABCDE', PL))
        A_L, B_L, C_L, D_L, E_L = PL

        # ---------------- strip macros ----------------
        rox = self._strip(20)
        rox += (1 - rox) % 3
        rx, rz = oscillator(b, rox, ZB - 100, 1)
        assert rx % 3 == 0, rx
        f.reserve_floor(rox - 3, rx + 2, ZB - 104, ZB - 97)
        self.pause.append((rox, Y, ZB - 103))
        rclk = f.new_lane('RCLK', rx, rz, loose=True)
        rng, rng_in = self._place_counter('RNG', 6)
        hx = self._place_updown('HX'); hy = self._place_updown('HY')
        tx = self._place_updown('TX'); ty = self._place_updown('TY')
        lq, l_in = self._place_counter('L', 4)
        fifo0, f0d = self._place_fifo('F0', STAGES); fifo1, f1d = self._place_fifo('F1', STAGES)
        assert self.strip_x <= f.x_max + 2, ('strip too wide', self.strip_x)
        self._drive_placeholder(rng_in, rclk)

        # ---------------- buttons -> REQ -> DIR (written at E) ----------------
        self.buttons = {}
        btn = {}
        for i, d in enumerate(('N', 'E', 'S', 'W')):
            x = -1050 + 3 * i
            b.button(x, Y, ZB - 40, 'south')
            self.buttons[d] = (x, Y, ZB - 40)
            btn[d] = f.new_lane(f'B{d}', x, ZB - 39)
        for l in btn.values(): l.keep = True
        nEW = self.G('nEW', [btn['E'], btn['W']], keep=True)
        nSW = self.G('nSW', [btn['S'], btn['W']])
        nNS = self.G('nNS', [btn['N'], btn['S']])
        D0 = self.G('REQD0', [nEW]); D1 = self.G('REQD1', [nSW], done=True)
        anyb = self.G('ANYB', [nEW, nNS], done=True)
        anyb.keep = True
        zreg = ZB + 40
        rq0 = self._reg('REQ0', D0, anyb, zreg)
        rq1 = self._reg('REQ1', D1, anyb, zreg)
        d0 = self._reg('DIR0', rq0, P[4], zreg + 30)
        d1 = self._reg('DIR1', rq1, P[4], zreg + 30)
        self.DIR = (d0, d1)
        hd = self._onehot('HD', d0, d1)

        # ---------------- GROW = (head == food), combinational ----------------
        self.growp = f.new_lane('GROWP', -1113, FAR, flow='north', loose=True)    # GROW (active-high)
        self.growpl = f.new_lane('GROWPL', -1116, FAR, flow='north', loose=True)  # NOT GROW
        wf = self.G('WFOOD', [E_L, self.growpl], x=self._slot_for([E_L, self.growpl]), keep=True)     # E & GROW
        fx = [self._reg(f'FX{i}', rng[i], wf, ZB + 130) for i in range(3)]
        fy = [self._reg(f'FY{i}', rng[3 + i], wf, ZB + 160) for i in range(3)]
        # the length counter's decoder (needed by GROW: no growth at the maximum length)
        lnq = [self.G(f'nL{i}', [lq[i]], keep=True) for i in range(4)]
        ldec = decoder4(f, 'LD', lq, lnq, None, values=range(1, STAGES + 1), keep_sel={STAGES})
        at_max = f.lanes[f'LD.S{STAGES}']                      # L == 12, active-high
        self.FX, self.FY = fx, fy
        nfx = [self.G(f'nFX{i}', [fx[i]], keep=True) for i in range(3)]
        nfy = [self.G(f'nFY{i}', [fy[i]], keep=True) for i in range(3)]

        def xnor_l(nm, a, na, c, nc):
            both = self.G(nm + '.ab', [na, nc])
            neither = self.G(nm + '.nn', [a, c])
            return self.G(nm + '.L', [both, neither], done=True)
        eqx_l = [xnor_l(f'EQX{i}', hx['Q'][i], hx['NQ'][i], fx[i], nfx[i]) for i in range(3)]
        eqy_l = [xnor_l(f'EQY{i}', hy['Q'][i], hy['NQ'][i], fy[i], nfy[i]) for i in range(3)]
        eqx = self.G('EQX', eqx_l); eqx_L = self.G('EQX_L', [eqx])
        eqy = self.G('EQY', eqy_l); eqy_L = self.G('EQY_L', [eqy])
        grow = self.G('GROW', [eqx_L, eqy_L, at_max], keep=True)  # head == food and L < 12
        grow_l = self.G('GROW_L', [grow], keep=True)
        self.grow, self.grow_l = grow, grow_l               # placeholders driven at the end

        # ---------------- head counters ----------------
        sx_ = self.G('STEPX', [A_L, hd['n0']]); stepx_l = self.G('STEPX_L', [sx_], done=True)
        sy_ = self.G('STEPY', [A_L, d0]);       stepy_l = self.G('STEPY_L', [sy_], done=True)
        updown3_logic(f, 'HX', hx, stepx_l, hd['E_L'])
        updown3_logic(f, 'HY', hy, stepy_l, hd['S_L'])

        # ---------------- length counter, FIFO, tail direction ----------------
        lcount = self.G('LCNT', [E_L, grow_l])
        self._drive_placeholder(l_in, lcount)
        self._drive_placeholder(f0d, d0)
        self._drive_placeholder(f1d, d1)
        for k in range(STAGES):
            fifo0.drive_write(k, lambda x, join, k=k: self.G(f'F0W{k}', [A_L], x=x, join=join, loose=True))
            fifo1.drive_write(k, lambda x, join, k=k: self.G(f'F1W{k}', [A_L], x=x, join=join, loose=True))
        td = []
        for bit, fifo in enumerate((fifo0, fifo1)):
            qls = [self.G(f'TQL{bit}_{k}', [fifo.q[k]]) for k in range(STAGES)]
            ins = list(ldec['SEL_L'].values()) + qls
            outx = self._slot_for(ins)
            out = self.G(f'TD{bit}', [ldec['SEL_L'][1], qls[0]], x=outx, keep=True)
            for k in range(1, STAGES):
                self.G(f'TD{bit}_{k}', [ldec['SEL_L'][k + 1], qls[k]], x=out.x, join=out)
            td.append(out)
        tdd = self._onehot('TDD', td[0], td[1])

        # ---------------- tail: erase pulse and step at B unless GROW ----------------
        per = self.G('PERASE', [B_L, self.growp], x=self._slot_for([B_L, self.growp]))   # B & !GROW, active-high
        ins = [B_L, tdd['n0'], self.growp]
        stx = self.G('STEPTX', ins, x=self._slot_for(ins)); stx_l = self.G('STEPTX_L', [stx], done=True)
        ins = [B_L, td[0], self.growp]
        sty = self.G('STEPTY', ins, x=self._slot_for(ins));  sty_l = self.G('STEPTY_L', [sty], done=True)
        updown3_logic(f, 'TX', tx, stx_l, tdd['E_L'])
        updown3_logic(f, 'TY', ty, sty_l, tdd['S_L'])

        # ---------------- wall check at A (registers still hold the old position) ----------------
        hx7 = self.G('HX7', [hx['NQ'][0], hx['NQ'][1], hx['NQ'][2]]); hx7_l = self.G('HX7_L', [hx7])
        hx0 = self.G('HX0', [hx['Q'][0], hx['Q'][1], hx['Q'][2]]);    hx0_l = self.G('HX0_L', [hx0])
        hy7 = self.G('HY7', [hy['NQ'][0], hy['NQ'][1], hy['NQ'][2]]); hy7_l = self.G('HY7_L', [hy7])
        hy0 = self.G('HY0', [hy['Q'][0], hy['Q'][1], hy['Q'][2]]);    hy0_l = self.G('HY0_L', [hy0])
        wins = [hd['E_L'], hd['W_L'], hd['S_L'], hd['N_L'], hx7_l, hx0_l, hy7_l, hy0_l, A_L]
        wallx = self._slot_for(wins)
        wall = self.G('WALL', [hd['E_L'], hx7_l, A_L], x=wallx, keep=True)
        self.G('WALL2', [hd['W_L'], hx0_l, A_L], x=wall.x, join=wall)
        self.G('WALL3', [hd['S_L'], hy7_l, A_L], x=wall.x, join=wall)
        self.G('WALL4', [hd['N_L'], hy0_l, A_L], x=wall.x, join=wall)

        # ---------------- death: collision bus or wall -> DEAD -> clock gate ----------------
        ncol = f.new_lane('NCOL', NCOL_X, BZ + 8 * PW + 1, flow='north', loose=True)
        ncol.keep = True
        nc = self.G('NC', [ncol, wall], x=self._slot_for([ncol, wall]))   # neither: active-low "any"
        dead_q, _ = self._rs('DEAD', nc, None)
        self._drive_placeholder(self.deadfb, dead_q)
        self.dead_q = dead_q
        b.lamp(dead_q.x, Y - 1, dead_q.z0)

        # ---------------- placeholders: feedback landings south of every consumer ----------------
        self._drive_placeholder(self.growp, self.grow)
        self._drive_placeholder(self.growpl, self.grow_l)

        # ---------------- X select lines: NOT gates at fixed slots, vias, lines ----------------
        xsrc = {'HX': (hx['Q'], hx['NQ']), 'TX': (tx['Q'], tx['NQ']), 'FX': (fx, nfx)}
        groups = []
        zline = -8
        k = 0
        self.xlines = {}
        for gname, key in (('HX', 'HX'), ('TX', 'TX'), ('FX', 'FX')):
            qs, nqs = xsrc[gname]
            lines = {}
            for n in BITS:
                bit = int(n[-1])
                src = nqs[bit] if n.startswith('Q') else qs[bit]     # NOT(NQ) = Q, NOT(Q) = NQ
                xs = self._free_slot_west(XLANE_X0 - 3 * k, inputs=[src])
                ln = self.G(f'X{gname}{n}', [src], x=xs, keep=True)
                lines[n] = zline
                self.xlines[(gname, n)] = (ln, zline)
                zline -= 4; k += 1
            cols = [(self.ports[key][i][0], bits_for(i)) for i in range(N)]
            groups.append((gname, lines, cols))
        self.xgroups = groups

        # ---------------- Y select lines and pulse lines: gates at fixed slots ----------------
        ysrc = {'HY': (hy['Q'], hy['NQ']), 'TY': (ty['Q'], ty['NQ']), 'FY': (fy, nfy)}
        ylanes = {}
        xs = YLINE_X0
        for pname, src in (('PSET', P[3]), ('PCHECK', P[2]), ('PERASE', per)):
            # the board pulse lines are active-LOW: NOT of the active-high pulse
            # (PSET_L = NOT D, PCHECK_L = NOT C, PERASE_L = NOT PERASE)
            ylanes[pname] = self.G(f'Y{pname}', [src], x=xs, keep=True)
            xs -= 6
        for gname in ('HY', 'TY', 'FY'):
            qs, nqs = ysrc[gname]
            for n in BITS:
                bit = int(n[-1])
                src = nqs[bit] if n.startswith('Q') else qs[bit]
                ylanes[gname + n] = self.G(f'Y{gname}{n}', [src], x=xs, keep=True)
                xs -= 6
        self.ylanes = ylanes
        rows = []
        for j in range(N):
            zj = BZ + PW * j
            rows.append((zj + ROW_Z[0], ['HY' + n for n in bits_for(j)] + ['PSET']))
            rows.append((zj + ROW_Z[1], ['HY' + n for n in bits_for(j)] + ['PCHECK']))
            rows.append((zj + ROW_Z[2], ['TY' + n for n in bits_for(j)] + ['PERASE']))
            rows.append((zj + ROW_Z[3], ['FY' + n for n in bits_for(j)]))
        self.yrows = rows
        # register the taps on the lanes and extend them to the last tap
        for z, names in rows:
            side = ROW_SIDE[(z - BZ) % PW]
            for nme in names:
                ln = ylanes[nme]
                ln.taps.add(z + side)
                ln.extend(z + side + 1)
        for (gname, n), (ln, zl) in self.xlines.items():
            ln.taps.add(zl); ln.extend(zl)

        # the fabric proper is finished: lay the lanes, then the hand-built parts
        f.build_lanes()
        near = [r for r in f.rects if r[1] >= -200 and r[3] < 0]
        assert max(r[3] for r in near) < -80, ('fabric rows reach the matrices', max(near, key=lambda r: r[3]))
        assert not b.collisions, ('overwritten blocks', b.collisions[:20])

        # X lines: via up from each source lane, level-2 row east to the matrix
        p = Pcb(b)
        for (gname, n), (ln, zl) in self.xlines.items():
            sx, _ = p.via_up(ln.x, zl)
            xline(b, zl, sx, BX - 7)
        xmatrix(b, BX, BZ, groups)
        # Y rows: taps from the lanes and the rows into the board
        for z, names in rows:
            side = ROW_SIDE[(z - BZ) % PW]
            joins = set()
            for nme in names:
                ytap(b, ylanes[nme].x, z, side)
                joins.add(ylanes[nme].x + 4)
            yrow(b, z, min(ylanes[nme].x for nme in names) + 4, BX - 1, joins)
        # collision bus: level 4 along z = 129 from the last column west, then down to the NCOL lane
        zb = BZ + 8 * PW + 1
        xs_col = [self.ports['COL'][i][0] for i in range(N)]
        since = 0
        for x in range(max(xs_col), NCOL_X + 4 - 1, -1):
            b.solid(x, 3, zb)
            since += 1
            if since >= 12 and x not in xs_col and x - 1 not in xs_col:
                b.repeater(x, L4, zb, 'east'); since = 0
            else:
                b.wire(x, L4, zb)
        x = NCOL_X + 4
        b.solid(x - 1, 2, zb); b.wire(x - 1, 3, zb)
        b.solid(x - 2, 1, zb); b.wire(x - 2, 2, zb)
        b.solid(x - 3, 0, zb); b.wire(x - 3, 1, zb)
        b.wire(NCOL_X, L0, zb)                      # = the NCOL lane's first cell (z0)
        assert not b.collisions, ('overwritten blocks (hand parts)', b.collisions[:20])
        self.stats = f.stats()
        print('stats', self.stats, 'blocks', len(w.blocks))


if __name__ == '__main__':
    m = Machine2()
