"""The Snake machine: assembles every subsystem on the fabric.

Layout (x east, z south):
  band W   x -1100..-780   buttons, clock, sequencer and the phase lanes
  strip    x  -760..-100   z < 0: the wide floor macros (RNG counter, the four
                           position counters, the length counter, both FIFO
                           staircases). Their outputs are lanes running south;
                           their inputs arrive from the south through feedback
                           landings and floor runs.
  region   z >= 0          every gate, packed by the fabric
  board    x -40..216      z 400..656, the 8x8 pixels

Phase plan (ticks after P1):  P1 0   DIR <- REQ
                              P2 16  FIFO shift, head step, wall check
                              P3 96  eat check -> GROW
                              P4 128 tail erase + tail step (unless GROW)
                              P5 200 collision check
                              P6 216 head set
                              P7 232 if GROW: length++, food <- RNG
                              P8 248 GROW reset
"""
from build import Builder, Y
from redtick import TickWorld
from fabric import Fabric
from pcb import Pcb, UP, MID
from cells import run_wire
from seq import tflop, pulse_gen, counter_up
from counters import updown3_place, updown3_logic
from decoder import decoder3, decoder4
from rslatch import rs_latch
from lockreg import reg_bit
from fifo import Fifo
from clockseq import oscillator, sequencer, sequencer_width
import gridcell

GX0, GZ0 = -40, 400          # grid origin
SPACING = (16, 80, 32, 188, 16, 16, 16)   # P5 waits for the column selects (~250 ticks)
FAR = 1500                   # placeholder lanes flowing north from here
STAGES = 12                  # direction FIFO depth = maximum snake length


class Machine:
    def __init__(self, clock_n=28):
        self.b = Builder(TickWorld()); self.w = self.b.w
        self.f = Fabric(self.b, z0=0)
        import os
        wide = os.environ.get('WIDE')          # WIDE=1: no width limit (logic checks only)
        self.f.x_min = -3000 if wide else -800
        self.f.x_max = 3000 if wide else GX0 + 8 * 32 + 60
        self.clock_n = clock_n
        self.regs = {}           # name -> storage repeater position (for initial state)
        self.build()

    def G(self, name, inputs, **kw):
        return self.f.gate(name, inputs, **kw)

    # ── register bit (gates around it) ──────────────────────────────────
    def _reg(self, name, d_lane, w_lane, z, x=None):
        """Register bit at slot x (D re-driven onto slot x, write pulse onto
        slot x+1 flowing north from a gate south of the register, Q on slot
        x+3). The two re-drive gates are placed automatically on whichever
        side is free; only the three register slots must be free."""
        f = self.f
        if x is None:
            def ok(xc):
                return not any(f.slot_blocked(s, 0) or not f.slot_free_below(s, 0)
                               for s in (xc, xc + 1, xc + 3))
            xe = ((max(d_lane.x, w_lane.x) + 8) // 3) * 3
            xw = ((min(d_lane.x, w_lane.x) - 12) // 3) * 3
            for _ in range(800):
                if ok(xe): x = xe; break
                if ok(xw): x = xw; break
                xe += 3; xw -= 3
            assert x is not None, name
        keep_out = (x, x + 1, x + 3)
        dl = self.G(name + '.dl', [d_lane], avoid=keep_out)
        D = self.G(name + '.d', [dl], x=x)
        wl = self.G(name + '.wl', [w_lane], avoid=keep_out)
        W = self.G(name + '.w', [wl], x=x + 1, out_flow='north', loose=True, z_min=z + 10)
        self.regs[name] = (x + 1, Y, z)
        return reg_bit(f, name, D, W, x, z, lamp=True)

    def _rs(self, name, s_src, r_src, ql=False):
        """RS latch whose SET (and RESET) lanes come from re-drive (inverting)
        gates at slots xa and xa+6, so `s_src` and `r_src` are ACTIVE-LOW
        lanes (low = set / reset). Q leaves on xa+9. The latch's own floor cells
        (xa-3..xa+9 at its z) must not be crossed by any lane, so every
        slot of that range has to be free from z=0 down, and the rectangle
        is reserved once the latch's z is known."""
        f = self.f
        span = range(-3, 10)
        def ok(xc):
            return xc % 3 == 0 and not any(f.slot_blocked(xc + o, 0) or not f.slot_free_below(xc + o, 0) for o in span)
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
        z = max(gs.z1, gr.z1 if gr else 0) + 6
        f.reserve_floor(xa - 3, xa + 9, z - 1, z + 3)
        q, qll = rs_latch(f, name, gs, gr, xa, z, ql=ql)
        return q, qll

    # ── strip macros ────────────────────────────────────────────────────
    def _strip(self, width):
        x0 = self.strip_x
        self.strip_x += width + 6
        return x0

    def _place_updown(self, name):
        x0 = self._strip(38) + 2
        x0 += (2 - x0) % 3                                   # x0 = 2 (mod 3): ports on the slot grid
        c = updown3_place(self.f, name, x0, -40)
        for i in range(3):
            self.regs[f'{name}{i}'] = (x0 + 12 * i + 4, Y, -40)
        return c

    def _place_counter(self, name, bits, in_lane=None):
        """Ripple counter in the strip. Its IN is a floor lane up its west
        side: fed directly by `in_lane` (a lane ending beside it) or, if None,
        a placeholder north-flowing lane to be driven later by feedback."""
        f, b = self.f, self.b
        width = 18 * bits + 4
        x0 = self._strip(width + 8) + 6
        x0 += (2 - x0) % 3                                   # ports on the slot grid
        z0 = -60
        f.reserve_floor(x0 - 6, x0 + width, z0 - 6, z0 + 12)
        c = counter_up(b, x0, z0, bits, lamps=True)
        ix, iz = c['IN']                                   # (x0-3, z0+8)
        b.wire(x0 - 4, Y, iz)
        inl = f.new_lane(f'{name}.IN', x0 - 5, FAR, flow='north', loose=True)
        inl.extend(iz)
        q = []
        for i, (qx, qz) in enumerate(c['Q']):
            q.append(f.new_lane(f'{name}{i}', qx, qz + 1, loose=True))
            self.regs[f'{name}{i}'] = (x0 + 18 * i + 4, Y, z0)
        return q, inl

    def _place_fifo(self, name, stages=15):
        f = self.f
        FW = 3 + 6 * stages + 6
        x0 = self._strip(FW + 3) + 3
        x0 -= x0 % 3                                         # D at x0-3, Q at x0+3+6k on the grid
        fz = -250
        f.reserve_floor(x0 - 4, x0 + FW, fz - 2, fz + 8 * stages + 6)
        dn = f.new_lane(f'{name}.D', x0 - 3, FAR, flow='north', loose=True)
        fifo = Fifo(f, name, dn, stages, x0, fz)
        return fifo, dn

    def _drive_placeholder(self, lane, src):
        """Feed a north-flowing placeholder lane from `src` by a feedback
        landing on it; the lane then really starts at the landing."""
        fx, fz = self.f.feedback(src, lane.x)
        lane.taps.add(fz)
        lane.z0 = fz

    def _onehot(self, name, d0, d1):
        d0.keep = True; d1.keep = True
        nd0 = self.G(f'{name}.n0', [d0], keep=True); nd1 = self.G(f'{name}.n1', [d1], keep=True)
        N = self.G(f'{name}.N', [d0, d1]);   NL = self.G(f'{name}.N_L', [N], keep=True)
        E = self.G(f'{name}.E', [nd0, d1]);  EL = self.G(f'{name}.E_L', [E], keep=True)
        S = self.G(f'{name}.S', [d0, nd1]);  SL = self.G(f'{name}.S_L', [S], keep=True)
        W = self.G(f'{name}.W', [nd0, nd1]); WL = self.G(f'{name}.W_L', [W], keep=True)
        return {'N_L': NL, 'E_L': EL, 'S_L': SL, 'W_L': WL, 'n0': nd0, 'n1': nd1}

    def _dec3(self, name, q, nq, slots=None, x_max=None, z_max=None):
        """z_max may be a list (per value) or a single value."""
        out = []
        for v in range(8):
            must_low = [nq[i] if (v >> i) & 1 else q[i] for i in range(3)]
            zm = z_max[v] if isinstance(z_max, list) else z_max
            sel = self.G(f'{name}.S{v}', must_low, z_max=zm)
            if slots is None:
                out.append(self.G(f'{name}.SL{v}', [sel], keep=True, x_max=x_max, z_max=zm))
            else:
                assert sel.x + 4 < slots[v], ('grid must move east of', name, sel.x)
                out.append(self.G(f'{name}.SL{v}', [sel], x=slots[v], loose=True, keep=True, z_max=zm))
        return out

    # ── build ───────────────────────────────────────────────────────────
    def build(self):
        b, f, w = self.b, self.f, self.w
        # reserve the board and its 32 driver rows
        f.reserve_floor(GX0 - 2, GX0 + 8 * 32 + 2, GZ0 - 2, GZ0 + 8 * 32 + 6)
        for j in range(8):
            for r in (0, 8, 16, 24):
                z = GZ0 + 32 * j + r
                f.rects.append((self.f.x_min - 10, GX0 + 260, z, z))

        # ---------------- band W: clock + sequencer ----------------
        cx, cz = oscillator(b, -800, -60, self.clock_n)
        assert cx % 3 == 0, ('CLK tap off the slot grid', cx)
        f.reserve_floor(-803, cx + 2, -64, -57)
        self.pause = [(-800, Y, -63)]                        # levers: ON = clock stopped
        clk = f.new_lane('CLK', cx, cz, loose=True)
        clk_l = self.G('CLK_L', [clk])
        # placeholder columns driven by feedback later (DEAD, GROW). Hand-placed
        # lanes MUST sit on the 3-slot grid: an off-grid column runs through
        # the cells of macros that assume the slots between lanes are empty.
        self.deadfb = f.new_lane('DEADFB', -831, FAR, flow='north', loose=True)
        self.growp = f.new_lane('GROWP', -834, FAR, flow='north', loose=True)   # GROW, driven later
        self.growpl = f.new_lane('GROWPL', -837, FAR, flow='north', loose=True) # NOT GROW, driven later
        g = self.G('G', [clk_l, self.deadfb], min_x=-790, side='east')
        seq_z = g.z0 + 8
        f.reserve_floor(g.x - 1, g.x + sequencer_width(8, SPACING) + 2, seq_z - 1, seq_z + 8)
        self.P = sequencer(f, g, g.x, seq_z, phases=8, spacing=list(SPACING))
        P = self.P
        PL = [self.G(f'P{k + 1}_L', [p], keep=True) for k, p in enumerate(P)]
        self.PL = PL

        # ---------------- strip: wide floor macros ----------------
        self.strip_x = -672
        rox = self._strip(20)
        rox += (1 - rox) % 3                               # tap (rox+5) on the slot grid
        rx, rz = oscillator(b, rox, -100, 1)               # period 24 ticks
        assert rx % 3 == 0, rx
        f.reserve_floor(rox - 3, rx + 2, -104, -97)
        self.pause.append((rox, Y, -103))
        rclk = f.new_lane('RCLK', rx, rz, loose=True)
        rng, rng_in = self._place_counter('RNG', 6)
        hx = self._place_updown('HX'); hy = self._place_updown('HY')
        tx = self._place_updown('TX'); ty = self._place_updown('TY')
        lq, l_in = self._place_counter('L', 4)
        fifo0, f0d = self._place_fifo('F0', STAGES); fifo1, f1d = self._place_fifo('F1', STAGES)
        assert self.strip_x < GX0 - 8, ('strip too wide', self.strip_x)
        # the RNG oscillator drives the RNG counter's IN placeholder
        self._drive_placeholder(rng_in, rclk)

        # ---------------- inputs: buttons -> request register ----------------
        self.buttons = {}
        btn = {}
        for i, d in enumerate(('N', 'E', 'S', 'W')):
            x = -816 + 3 * i                                 # slot-grid columns
            b.button(x, Y, -40, 'south')
            self.buttons[d] = (x, Y, -40)
            btn[d] = f.new_lane(f'B{d}', x, -39)
        for l in btn.values(): l.keep = True
        nEW = self.G('nEW', [btn['E'], btn['W']], keep=True)
        nSW = self.G('nSW', [btn['S'], btn['W']])
        nNS = self.G('nNS', [btn['N'], btn['S']])
        D0 = self.G('REQD0', [nEW]); D1 = self.G('REQD1', [nSW], done=True)
        anyb = self.G('ANYB', [nEW, nNS], done=True)
        anyb.keep = True
        zreg = 40
        rq0 = self._reg('REQ0', D0, anyb, zreg)
        rq1 = self._reg('REQ1', D1, anyb, zreg)
        d0 = self._reg('DIR0', rq0, P[0], zreg + 30)
        d1 = self._reg('DIR1', rq1, P[0], zreg + 30)
        self.DIR = (d0, d1)
        hd = self._onehot('HD', d0, d1)

        # ---------------- food registers (written by P7 & GROW) ----------------
        wf = self.G('WFOOD', [PL[6], self.growpl], keep=True)  # P7 & GROW (both inputs active-low)
        fx = [self._reg(f'FX{i}', rng[i], wf, 230) for i in range(3)]
        fy = [self._reg(f'FY{i}', rng[3 + i], wf, 260) for i in range(3)]
        self.FX, self.FY = fx, fy
        nfx = [self.G(f'nFX{i}', [fx[i]], keep=True) for i in range(3)]
        nfy = [self.G(f'nFY{i}', [fy[i]], keep=True) for i in range(3)]

        # ---------------- decoders and board-row drivers (early: they need
        # rows north of the board, before the region fills up) ----------------
        HX_L = self._dec3('HXD', hx['Q'], hx['NQ'], [GX0 + 32 * v for v in range(8)], z_max=GZ0 - 10)
        TX_L = self._dec3('TXD', tx['Q'], tx['NQ'], [GX0 + 32 * v + 3 for v in range(8)], z_max=GZ0 - 10)
        FX_L = self._dec3('FXD', fx, nfx, [GX0 + 32 * v + 6 for v in range(8)], z_max=GZ0 - 10)
        rowz = [GZ0 + 32 * j for j in range(8)]
        hyd = self._dec3('HYD', hy['Q'], hy['NQ'], z_max=[zz - 12 for zz in rowz])
        tyd = self._dec3('TYD', ty['Q'], ty['NQ'], z_max=[zz + 16 - 12 for zz in rowz])
        fyd = self._dec3('FYD', fy, nfy, x_max=GX0 - 9, z_max=[zz + 24 - 6 for zz in rowz])   # board rows
        pset_l = PL[5]; pcheck_l = PL[4]
        per = self.G('PERASE', [PL[3], self.growp]); perase_l = self.G('PERASE_L', [per], keep=True)
        HYS_L, HYC_L, TYE_L = [], [], []
        for j in range(8):
            zj = GZ0 + 32 * j
            a = self.G(f'HYS{j}', [hyd[j], pset_l], z_max=zj - 10)
            HYS_L.append(self.G(f'HYS_L{j}', [a], keep=True, x_max=GX0 - 9, z_max=zj - 6))
            c = self.G(f'HYC{j}', [hyd[j], pcheck_l], z_max=zj + 8 - 10)
            HYC_L.append(self.G(f'HYC_L{j}', [c], keep=True, x_max=GX0 - 9, z_max=zj + 8 - 6))
            t = self.G(f'TYE{j}', [tyd[j], perase_l], z_max=zj + 16 - 10)
            TYE_L.append(self.G(f'TYE_L{j}', [t], keep=True, x_max=GX0 - 9, z_max=zj + 16 - 6))

        # ---------------- head counters ----------------
        sx_ = self.G('STEPX', [PL[1], hd['n0']]); stepx_l = self.G('STEPX_L', [sx_], done=True)
        sy_ = self.G('STEPY', [PL[1], d0]);       stepy_l = self.G('STEPY_L', [sy_], done=True)
        updown3_logic(f, 'HX', hx, stepx_l, hd['E_L'])
        updown3_logic(f, 'HY', hy, stepy_l, hd['S_L'])

        # ---------------- eat detection ----------------
        def xnor_l(nm, a, na, c, nc):
            both = self.G(nm + '.ab', [na, nc])
            neither = self.G(nm + '.nn', [a, c])
            return self.G(nm + '.L', [both, neither], done=True)
        eqx_l = [xnor_l(f'EQX{i}', hx['Q'][i], hx['NQ'][i], fx[i], nfx[i]) for i in range(3)]
        eqy_l = [xnor_l(f'EQY{i}', hy['Q'][i], hy['NQ'][i], fy[i], nfy[i]) for i in range(3)]
        eqx = self.G('EQX', eqx_l); eqx_L = self.G('EQX_L', [eqx])
        eqy = self.G('EQY', eqy_l); eqy_L = self.G('EQY_L', [eqy])
        eat = self.G('EAT', [eqx_L, eqy_L, PL[2]])

        # length counter counts P7 & GROW; its decoder gives the FIFO tap
        lcount = self.G('LCNT', [PL[6], self.growpl])         # P7 & GROW
        self._drive_placeholder(l_in, lcount)
        lnq = [self.G(f'nL{i}', [lq[i]], keep=True) for i in range(4)]
        ldec = decoder4(f, 'LD', lq, lnq, None, values=range(1, STAGES + 1), keep_sel={STAGES})
        at_max = f.lanes[f'LD.S{STAGES}']
        eat_l = self.G('EAT_L', [eat])
        eatg = self.G('EATG', [eat_l, at_max])              # EAT & L != max, active high
        eatg_l = self.G('EATG_L', [eatg])
        grow, _ = self._rs('GROW', eatg_l, PL[7])            # set on eat, reset at P8
        grow.keep = True
        self._drive_placeholder(self.growp, grow)             # feedback rows do not invert
        grow_l = self.G('GROW_L', [grow], keep=True)
        self._drive_placeholder(self.growpl, grow_l)

        # ---------------- direction FIFO, tail direction ----------------
        self._drive_placeholder(f0d, d0)
        self._drive_placeholder(f1d, d1)
        shift_l = PL[1]
        for k in range(STAGES):
            fifo0.drive_write(k, lambda x, join, k=k: self.G(f'F0W{k}', [shift_l], x=x, join=join, loose=True))
            fifo1.drive_write(k, lambda x, join, k=k: self.G(f'F1W{k}', [shift_l], x=x, join=join, loose=True))
        td = []
        for bit, fifo in enumerate((fifo0, fifo1)):
            qls = [self.G(f'TQL{bit}_{k}', [fifo.q[k]]) for k in range(STAGES)]
            outx = self._free_slot(max(l.x for l in list(ldec['SEL_L'].values()) + qls) + 5)
            out = self.G(f'TD{bit}', [ldec['SEL_L'][1], qls[0]], x=outx, keep=True)
            for k in range(1, STAGES):
                self.G(f'TD{bit}_{k}', [ldec['SEL_L'][k + 1], qls[k]], x=out.x, join=out)
            td.append(out)
        tdd = self._onehot('TDD', td[0], td[1])

        # ---------------- tail counters ----------------
        stx = self.G('STEPTX', [PL[3], tdd['n0'], grow]); stx_l = self.G('STEPTX_L', [stx], done=True)
        sty = self.G('STEPTY', [PL[3], td[0], grow]);      sty_l = self.G('STEPTY_L', [sty], done=True)
        updown3_logic(f, 'TX', tx, stx_l, tdd['E_L'])
        updown3_logic(f, 'TY', ty, sty_l, tdd['S_L'])

        # wall check at P2 (decoders still show the position before the move)
        wallx = self._free_slot(GX0 + 8 * 32 + 6)           # east of the board: clear of every input
        wall = self.G('WALL', [hd['E_L'], HX_L[7], PL[1]], x=wallx, keep=True)
        self.G('WALL2', [hd['W_L'], HX_L[0], PL[1]], x=wall.x, join=wall)
        self.G('WALL3', [hd['S_L'], hyd[7], PL[1]], x=wall.x, join=wall)
        self.G('WALL4', [hd['N_L'], hyd[0], PL[1]], x=wall.x, join=wall)

        # ---------------- the board ----------------
        p = Pcb(b)
        self.cells = {}
        col_lanes = {}
        for i in range(8):
            cx_ = GX0 + 32 * i
            for lane in (HX_L[i], TX_L[i], FX_L[i]):
                for j in range(8):
                    for zz in (3, 11, 19, 27):
                        lane.taps.add(GZ0 + 32 * j + zz)
                lane.extend(GZ0 + 8 * 32)
            dead = f.new_lane(f'COL{i}', cx_ + 27, GZ0 - 1, loose=True)
            for j in range(8):
                dead.taps.add(GZ0 + 32 * j + 11)
            dead.extend(GZ0 + 8 * 32 + 1)
            col_lanes[i] = dead
        for j in range(8):
            cz_ = GZ0 + 32 * j
            for lane, r in ((HYS_L[j], 0), (HYC_L[j], 8), (TYE_L[j], 16), (fyd[j], 24)):
                z = cz_ + r
                assert lane.x < GX0 - 8, (lane.name, lane.x)
                lane.taps.add(z); lane.extend(z)
                start = p.via_up(lane.x, z)
                p.row(start[0], GX0 - 2, z, rep_every=8)
            gridcell.through_rows(p, GX0, GX0 + 8 * 32, cz_)
        for i in range(8):
            for j in range(8):
                self.cells[(i, j)] = gridcell.cell(b, GX0 + 32 * i, GZ0 + 32 * j)

        # ---------------- death ----------------
        anyc = self.G('ANYC0', [col_lanes[0], col_lanes[1], col_lanes[2]])
        anyc2 = self.G('ANYC1', [col_lanes[3], col_lanes[4], col_lanes[5]])
        anyc3 = self.G('ANYC2', [col_lanes[6], col_lanes[7], wall])
        nc = self.G('NCOL', [anyc, anyc2, anyc3])            # any collision or wall, active high
        ncl = self.G('NCOL_L', [nc])
        dead_q, _ = self._rs('DEAD', ncl, None)
        self._drive_placeholder(self.deadfb, dead_q)
        self.dead_q = dead_q
        b.lamp(dead_q.x, Y - 1, dead_q.z0)
        f.build_lanes()
        assert not b.collisions, ('blocks overwritten by other blocks', b.collisions[:20])
        self.stats = f.stats()
        print('stats', self.stats, 'blocks', len(w.blocks))

    def _free_slot(self, x):
        x = ((x + 2) // 3) * 3
        while self.f.by_slot.get(x) or self.f.slot_blocked(x):
            x += 3
        return x


if __name__ == '__main__':
    m = Machine()
