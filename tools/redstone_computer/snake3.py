"""Snake v3: the v2 architecture (board.py, matrix.py, snake2.py) with an
explicit floor plan so that every signal path is short.

x (west -> east):
  control band  RNG | L | F0 | F1 | DEAD latch gap | placeholders
  X band        HX | TX | FX registers            (X select lines: rows east at z = -8..-76)
  clock band    oscillator, clock gate, sequencer (P lanes flow south from here)
  Y band        buttons, REQ/DIR, HY | TY | FY registers | pulse lines | GROW placeholders | NCOL
  board         (0, 0) .. (127, 127)
The Y select lines ARE the counters' Q/NQ lanes (NQ lanes tap east, Q lanes
tap west, so lanes 3 apart never collide); the X select lines are level-2
rows taken off the HX/TX/FX lanes with a via at their line's z.
Phases: A head step + wall; B tail erase unless GROW, DIR <- REQ; C check + L++ if GROW;
D set; E tail step unless GROW, food <- RNG if GROW, FIFO shift (NOT(DIR) in).
"""
import build
from build import Builder, Y
from redtick import TickWorld
from fabric import Fabric
from pcb import Pcb
from counters import updown3_place, updown3_logic, PITCH
from seq import counter_up
from rslatch import rs_latch
from lockreg import reg_bit
from fifo import Fifo
from tailmux import tailmux, tap_cells, row_z
from clockseq import oscillator, sequencer, sequencer_width
from board import board, N
from matrix import xmatrix, xline, xvia_up, ytap, ytap_join, yrow, ROW_SIDE
from pixel import W as PW, L0, L2, L4, ROW_Z
from screen import screen
from deck import deck
from panel import panel as make_panel, PITCH as PPITCH, PORT_W, PORT_N, SIZE as PSIZE, ROW_Z as PROW_Z

BX, BZ = 0, 0
PANEL = False                    # plus machines: the board is a display-block panel (panel.py); vanilla: lamp pixels (board.py)
DECK_ORIGIN = (-14, 168)         # the deck stands 30 blocks east of the wall (panel.WALL_X), looking west at it
ZB = -480                        # fabric row base
PZ = -92                         # the control platform (buttons, start lever, death lamp) at z = PZ
FAR = 10 ** 5
STAGES = 12
SPACING = (312, 16, 32, 168)     # A->B, B->C, C->D, D->E: GROW reaches its consumers by ~A+270 (Y moves are the slow case); erase lands B+160, check C+178, set D+160.
                                 # D->E is wide because the FIFO shifts at E from NOT(DIR): DIR is written B+64, the FIFO input follows ~120 ticks later
                                 # and the write pulses arrive E+134; the mux/TD path back to the tail gates takes ~165 ticks and must be done by the next B.
# redstone_plus with blue (zero-delay) gates — measured with play2 --rec:
#   head step done A+18, GROW A+28, a deck press reaches DIR ~34 ticks after the press,
#   the board column select A+88 (the pixel matrix's diode repeaters: the slow path
#   left), DIR B+14, L++ C+30, tail mux C+68, FIFO/food writes and EQ/GROW E+32.
# A->B 56 covers GROW and a press at A+8; C at 112 covers the column select; E at
# C+88 covers the tail mux; the next A comes 36 ticks after E, overlapping the tail
# bookkeeping (valid at E+60 = next A+24, before next B) with the next head step.
SPACING_PLUS = (56, 56, 16, 72)
CLOCK_N_PLUS = 14                 # active repeaters: period 16n + 12 = 236 ticks (vanilla machine: 596); the
                                  # oscillator keeps its 19-cell length so the clock band's layout does not move
# harness read points (play2): press / late read / step length, per timing
TIMING = {'PRESS': 100, 'LATE': 250, 'SETTLE': 585}
TIMING_PLUS = {'PRESS': 8, 'LATE': 40, 'SETTLE': 200, 'WALLFOOD': 'late'}   # the frame buffer is presented at E
REG_DZ = 0                        # (plus builds once pushed the registers 40 south for the DIR forcing joins; DIR is now forced through REQ)
# what the restart forces: everything a lock latch holds is forced to 1 (an OR joined onto its
# lane is the one forcing that costs no D-gate input): head and tail (7, 7), direction west
# (bits 1,1), food (0, 0) (its registers store NOT the coordinate). play2/write_snake_world
# start plus machines from m.init.
INIT_PLUS = {'head': (7, 7), 'food': (0, 0), 'dir': 3}
INIT_VANILLA = {'head': (3, 3), 'food': (5, 3), 'dir': 1}   # late read before B erases the tail (A+56); the wall food has caught up by the main read
BITS = ['Q0', 'NQ0', 'Q1', 'NQ1', 'Q2', 'NQ2']
XMIN = -890                       # west edge of the fabric's placeable range (restart logic needs more: see __init__)
NCOL_X = -12


def bits_for(v):
    return ['NQ%d' % b if (v >> b) & 1 else 'Q%d' % b for b in range(3)]


class Machine3:
    def __init__(self, clock_n=None, plus=False, blue=None):
        build.set_plus(plus, blue)
        global XMIN
        global REG_DZ, BZ, NCOL_X, PANEL
        REG_DZ = 8 if plus else 0          # plus: the restart and PRESENT gates take rows north of the registers
        PANEL = bool(plus)
        BZ = 16 if PANEL else 0            # the panel's column ports sit at z 1..15; the x-matrix columns end at z = 0
        NCOL_X = -18 if PANEL else -12     # west of the panel's row ports (x -14..-1)
        self.zb = (BZ + PSIZE + 1) if PANEL else (BZ + 8 * PW + 1)          # the NCOL lane's start (panel: the COL merge line's z)
        self.zclear = (BZ + PSIZE + 6) if PANEL else (BZ + 4)              # where the YCLEAR lane ends
        XMIN = -960 if plus else -890             # plus: a strip west of the length counter for the DEAD and restart latches (13 free columns each)
        self.b = Builder(TickWorld()); self.w = self.b.w
        self.w.no_decay = bool(plus)
        self.plus = bool(plus)
        self.spacing = SPACING_PLUS if plus else SPACING
        self.timing = dict(TIMING_PLUS if plus else TIMING)
        self.init = dict(INIT_PLUS if plus else INIT_VANILLA)
        self.clock_active = CLOCK_N_PLUS if plus else None
        if clock_n is None:
            clock_n = 19 if plus else 36
        self.f = Fabric(self.b, z0=ZB)
        self.f.x_min = XMIN
        self.f.x_max = -48                # automatic placement stays west of the hand-assigned east end
        self.clock_n = clock_n
        self.regs = {}
        self.build()

    def G(self, name, inputs, **kw):
        if 'x' not in kw and 'avoid' not in kw:
            kw['avoid'] = tuple(self.reg_slots | self.reserved)   # automatic placement keeps off hand-assigned columns
        return self.f.gate(name, inputs, **kw)

    # ── helpers ─────────────────────────────────────────────────────────
    ZSOUTH = 134                      # rows below the board rect (z 0..131): every finished lane's slot is free again down here

    def _build_reset_logic(self, lq, lnq, rng0, l_in, hd, d0, d1, f0d, f1d):
        """Restart (plus machines), built LAST and SOUTH of everything (rows at
        z >= ZSOUTH, where the hub's columns are free again below the lanes
        that ended). RSTH = an RS latch SET by the lever (PAUSE) and RESET when
        the lever is off again AND the length counter is back at 1; while it
        is on the clock gate stays shut (HOLD), every register's write is held
        open with its D forced (RSTX), the counters walk to their start values
        (_walk_join), the direction FIFOs shift the initial direction in, the
        request and death latches are forced, the board is cleared (YCLEAR),
        and the length counter is pulsed at RCLK pace until it reads 1 — so a
        lever flipped on and straight off still completes the reset before the
        game restarts."""
        f = self.f
        ZS = self.ZSOUTH
        f.x_min, f.x_max = XMIN, -48
        W = lambda name, ins, **kw: self.G(name, ins, z_min=ZS, **kw)
        # PAUSE flows north from the lever platform: its inverter must sit north of it, everything else runs south from there
        pause_l = self.G('PAUSE_L', [self.pause_lane], x=self._any_slot([self.pause_lane], prefer=-600), keep=True)
        pause2 = W('PAUSE2', [pause_l], keep=True)                              # PAUSE again, flowing south
        u23 = W('L.23', [lq[2], lq[3]])
        t23 = W('L.23_L', [u23])                                                # L2 | L3
        eq1 = W('L.EQ1', [lnq[0], lq[1], t23], keep=True)                       # L == 1
        eq1_l = W('L.EQ1_L', [eq1], keep=True)
        # RESET = lever off & L==1 & every counter at its start value (a lever flipped on and off
        # before the walk is over must not release the reset early)
        r1 = W('RSTHQ.r1', [eq1_l, pause2])                                     # L==1 & lever off
        r1_l = W('RSTHQ.r1L', [r1])
        nat7 = {k: W(f'RSTHQ.n{k}', [self.at7[k]]) for k in ('HX', 'HY', 'TX', 'TY')}
        r2 = W('RSTHQ.r2', [nat7['HX'], nat7['HY'], nat7['TX']])
        r2_l = W('RSTHQ.r2L', [r2])
        r = W('RSTHQ.r', [r1_l, r2_l, nat7['TY']])
        rsth_rs = W('RSTHQ.rs', [r])                                            # active-low RESET source
        rsthq, _ = self._rs2('RSTHQ', pause_l, rsth_rs, self._south_latch_x(XMIN + 18), z_min=ZS)
        rsthq.keep = True
        rsth_l = W('RSTH_Lg', [rsthq], keep=True)
        with build.red_torches():                                               # 4 x 2 ticks: RSTH delayed 8
            d = rsthq
            for i in range(4):
                d = W(f'RSTD{i}', [d], keep=True)
        rstd = d
        rstx_l = W('RSTX_Lg', [rsthq, rstd], keep=True)                          # !(RSTH | RSTD)
        rstx = W('RSTXg', [rstx_l], keep=True)                                  # RSTH stretched by 8 ticks
        pr = W('PAUSE_RST', [pause2, rsthq])
        hold = W('HOLDg', [pr], keep=True)                                      # PAUSE | RSTH
        # the clock gate must open on a CLK rising edge, never mid-pulse (a truncated first pulse
        # shifts the whole cycle late and its E collides with the next A): HOLDS = HOLD | (HOLDS & CLK),
        # a latch that keeps holding until the clock is low. HOLDS_L feeds back through a placeholder.
        hsl_ph = f.new_lane('HOLDS_L', self._slot_for([], prefer=XMIN + 60), FAR, flow='north', loose=True)
        hsl_ph.keep = True
        # CLK's own inverter ends at the sequencer (its column is the sequencer's), so the hold reads CLK
        # through a second inverter whose rows sit north of the sequencer on a free western column
        clk2_l = self.G('CLK_L2', [self.clk], x=self._slot_west_of([self.clk], -560, -482, x_lo=XMIN), keep=True, z_min=-490, z_max=-482)
        ha = W('HOLDS.a', [hsl_ph, clk2_l])                                     # HOLDS & CLK
        hsl = W('HOLDS_Lg', [hold, ha])                                         # !(HOLD | (HOLDS & CLK))
        self._drive_placeholder(hsl_ph, hsl)
        hs = W('HOLDSg', [hsl_ph], keep=True)                                   # HOLDS
        # the walk clock: a 4-tick pulse on every rising edge of RNG bit 0 (period 40). The toggle
        # flip-flops need a write pulse shorter than their own toggle (~6 ticks): the phase pulses
        # are 4 ticks, RCLK's 10 would toggle them twice.
        r0l = W('RCLK0_L', [rng0], keep=True)                                   # NOT rng0 (blue: no delay)
        with build.red_torches():
            rd1 = W('RCLKD1', [rng0])                                           # 2 ticks
            rd2 = W('RCLKD2', [rd1])                                            # rng0 delayed 4
        rp = W('RPULSE', [r0l, rd2], keep=True)                                 # rng0 & !rng0_d4
        rclk_l = W('RCLK_Lg', [rp], keep=True)                                  # NOT pulse
        linr = W('L.INR', [rsth_l, eq1, rclk_l])                                # pulses L++ while L != 1
        # the FIFOs shift "west" in (stored NOT dir = 0, 0)
        src0 = W('F0.dr', [d0, self.rstx])                                      # !d0 & !RSTX
        src1 = W('F1.dr', [d1, self.rstx])                                      # !d1 & !RSTX
        self._drive_placeholder(f0d, src0)
        self._drive_placeholder(f1d, src1)
        for ph, src in ((self.rsth, rsthq), (self.rsth_l, rsth_l), (self.rstx, rstx), (self.rclk_l, rclk_l), (self.hold, hs), (self.rstx_l, rstx_l)):
            self._drive_placeholder(ph, src)
        self._drive_placeholder(l_in, linr)

    def _slot_west_of(self, inputs, x_hi, z, x_lo=-620):
        """The first column at or west of x_hi that a gate with rows at z can
        take (its lanes all done north of z) and that clears the inputs."""
        f = self.f
        x = x_hi - (x_hi % 3)
        while x >= x_lo:
            if (f.slot_free_below(x, z) and not f.slot_blocked(x, z, 'south') and x not in self.reserved
                    and all(abs(x - l.x) > 4 for l in inputs) and all(abs(x - r) > 2 for r in self.reg_slots)):
                return x
            x -= 3
        raise AssertionError(('no column west of', x_hi, 'free at', z))

    def _south_latch_x(self, xa):
        """The first column >= xa whose 13-slot span (xa-3 .. xa+9) is free
        south of the board, for an RS latch placed at z >= ZSOUTH."""
        f = self.f
        z = self.ZSOUTH + 40
        while not all(f.slot_free_below(xa + o, z) and not f.slot_blocked(xa + o, z, 'south')
                      and (xa + o) not in self.reserved for o in range(-3, 10)):
            xa += 3
            assert xa < -60, 'no room for a latch south of the fabric'
        return xa

    def _west_slot(self, inputs, z_min=None):
        """A free slot in the strip west of the fabric (XMIN .. -890)."""
        f = self.f
        saved = (f.x_min, f.x_max)
        f.x_min, f.x_max = XMIN, -48
        try:
            x = self._slot_for(inputs, prefer=XMIN + 3, z_min=z_min)
            assert x < -890, x
            return x
        finally:
            f.x_min, f.x_max = saved

    def _any_slot(self, inputs, prefer, z_min=None):
        """A free slot anywhere in the vanilla fabric (-890 .. -48)."""
        f = self.f
        saved = (f.x_min, f.x_max)
        f.x_min, f.x_max = -890, -48
        try:
            return self._slot_for(inputs, prefer=prefer, z_min=z_min)
        finally:
            f.x_min, f.x_max = saved

    def _west_gate(self, name, inputs, **kw):
        """A restart gate in the strip west of the fabric (XMIN .. -890)."""
        f = self.f
        saved = (f.x_min, f.x_max)
        f.x_min, f.x_max = XMIN, -48
        try:
            x = self._slot_for(inputs, prefer=XMIN + 3)
            assert x < -890, (name, x)
            return self.G(name, inputs, x=x, **kw)
        finally:
            f.x_min, f.x_max = saved

    def _board_clear(self, b):
        """Restart: YCLEAR (= RSTH) up a ramp east of its lane, a level-4 trunk
        south along x = -23 and a branch east into every pixel row's clear line
        (board.clear_lines). Glowstone under everything."""
        from board import clear_lines, N as BN
        from pixel import W as PW_
        ln = self.yclear
        z = BZ + 4                    # between the HYS (z0) and HYC (z0+8) rows: the ramp's cells at levels 1..3 must not touch a level-2 row
        assert ln.zrange()[1] == z, ('YCLEAR must be extended to the ramp before build_lanes', ln.zrange())
        xc = ln.x
        b.solid(xc + 1, Y, z); b.wire(xc + 1, Y + 1, z)
        b.glow(xc + 2, Y + 1, z); b.wire(xc + 2, Y + 2, z)
        b.glow(xc + 3, Y + 2, z); b.wire(xc + 3, Y + 3, z)
        tx = xc + 4
        for zz in range(z, BZ + PW_ * (BN - 1) + 15 + 1):
            b.glow(tx, Y + 3, zz); b.wire(tx, Y + 4, zz)
        for j in range(BN):
            zz = BZ + PW_ * j + 15
            for x in range(tx + 1, BX - 1):
                b.glow(x, Y + 3, zz); b.wire(x, Y + 4, zz)
        clear_lines(b, BX, BZ)

    def _panel_clear(self, b):
        """Panel: YCLEAR (= RSTH) climbs to level 8 past its lane's end (south
        of the panel), runs east over everything and north into the panel's
        CLEAR net (panel.py: the row-join line at x0 - 1)."""
        ln = self.yclear
        zc = self.zclear
        assert ln.zrange()[1] == zc, ('YCLEAR must be extended to the clear run before build_lanes', ln.zrange())
        xj, yj, zj = self.ports['CLEAR']
        for k in range(1, yj + 1):                   # rise south: dust (x, k, zc + k) on a solid
            b.solid(ln.x, k - 1, zc + k); b.wire(ln.x, k, zc + k)
        ze = zc + yj
        for x in range(ln.x + 1, xj + 1):
            b.glow(x, yj - 1, ze); b.wire(x, yj, ze)
        for z in range(ze - 1, zj, -1):
            b.glow(xj, yj - 1, z); b.wire(xj, yj, z)

    def _panel_present(self, b):
        """Panel: the PRESENT lane's end runs east at level 0 (on glowstone)
        along the wall's south side into the PRESENT staircase (panel.py)."""
        ln = self.present
        xp, yp, zp = self.ports['PRESENT']
        assert ln.zrange()[1] == zp and yp == 0, (ln.zrange(), self.ports['PRESENT'])
        for x in range(ln.x + 1, xp):
            b.glow(x, -1, zp); b.wire(x, 0, zp)

    def _panel_col(self, b):
        """Panel: the merged check output (level 12, south of the panel) runs
        west over the row ports and steps down, one level per cell going
        west, onto the NCOL lane's first cell (z = the merge line's z; the
        rows end further north, so the lane crosses none of them here)."""
        x_out, yc, zc = self.ports['COL']
        xn = NCOL_X
        xs = xn + yc                                  # the last level-yc cell, on a solid
        assert xs < x_out, (xs, x_out)
        for x in range(x_out - 1, xs, -1):
            b.glow(x, yc - 1, zc); b.wire(x, yc, zc)
        b.solid(xs, yc - 1, zc); b.wire(xs, yc, zc)                        # solid: the step below reads it through a conductor
        for k in range(1, yc):
            b.solid(xs - k, yc - k - 1, zc); b.wire(xs - k, yc - k, zc)
        assert self.zb == zc, (self.zb, zc)

    def _walk_join(self, name, c, step_lane):
        """Restart (plus): while RSTH is on and the counter is not at its start
        value, RCLK pulses are joined onto its STEP lane, so it walks (mod 8)
        to the start value — up to 7 steps of 16 ticks. Nothing on the write
        lanes; the STEP lane is joined before its consumer closes it."""
        if not build.PLUS:
            return
        value = 7                                                   # head and tail start at (7, 7)
        sel = [c['NQ'][i] if (value >> i) & 1 else c['Q'][i] for i in range(3)]   # all low exactly at `value`
        # south of the fabric (see _build_reset_logic): the counter's Q/NQ lanes simply run on south
        eq = self.G(f'{name}.AT7', sel, z_min=self.ZSOUTH, keep=True)
        self.at7[name] = eq
        self.G(f'{name}.WALK', [self.rsth_l, eq, self.rclk_l], x=step_lane.x, join=step_lane, loose=True, z_min=self.ZSOUTH)

    @build.red          # flip-flops keep the delayed torch (see build.red_torches)
    def _reg(self, name, d_lane, w_lane, z, x, direct=False, init=None):
        """With direct=True the register stores NOT(d_lane) (one gate). `init`
        (plus machines): the stored value the restart forces — the write is
        held open while RSTH is on and the D path carries `init` while RSTX
        (RSTH stretched by 8 ticks) is on, so the latch locks on it. Forcing
        is done by JOINING a gate onto an existing lane (dust ORs), never by
        adding an input to the D gate: an extra row would push that gate south
        of the register it must sit north of. init=1 ORs RSTX onto D; init=0
        (non-direct only) ORs RSTX onto the D gate's inverted input."""
        f = self.f
        keep_out = tuple(self.reg_slots)                 # every planned register column
        rst = init is not None and build.PLUS            # write held open while RSTH
        force = rst and init != 'hold'                   # 'hold': the D path is forced upstream (DIR: through the REQ latches)
        # the D gate must sit NORTH of the register (its via support lands on x+1, the register's
        # lock column): with more rows in the machine the allocator can drift past it, so say so
        zmax = dict(z_max=z - 3) if build.PLUS else {}
        if direct:
            assert not rst or init, (name, 'a direct register can only be forced to 1: choose the initial food accordingly')
            D = self.G(name + '.d', [d_lane], x=x, **zmax)
        else:
            dl = self.G(name + '.dl', [d_lane], avoid=keep_out)
            if force and not init:
                self.G(name + '.dlr', [self.rstx_l], x=dl.x, join=dl, loose=True)     # dl | RSTX -> D = d & !RSTX
            D = self.G(name + '.d', [dl], x=x, **zmax)
        if force and init:                               # north of the register: its via support sits on x+1
            self.G(name + '.dr', [self.rstx_l], x=x, join=D, loose=True, z_max=z - 3)  # D | RSTX
        wl = self.G(name + '.wl', [w_lane] + ([self.rsth] if rst else []), avoid=keep_out)   # W | RSTH
        Wl = self.G(name + '.w', [wl], x=x + 1, out_flow='north', loose=True, z_min=z + 10)
        self.regs[name] = (x + 1, Y, z)
        return reg_bit(f, name, D, Wl, x, z, lamp=True)

    def _rs_at(self, name, s_src, xa, z_min=None):
        """RS latch with its SET re-drive gate at slot xa (13 free slots
        xa-3..xa+9 needed); `s_src` is active-low."""
        f = self.f
        keep_out = tuple(xa + o for o in range(-3, 10))
        gs = self.G(name + '.S', [s_src], x=xa, avoid=keep_out, z_min=z_min)
        z = gs.z1 + 6
        f.reserve_floor(xa - 3, xa + 9, z - 1, z + 3)
        return rs_latch(f, name, gs, None, xa, z, ql=False)

    def _rs2(self, name, s_src, r_src, xa, z_min=None):
        """RS latch (rslatch.rs_latch) with SET and RESET re-drive gates at
        slots xa and xa+6 (13 free slots xa-3..xa+9); both sources
        active-low. Q leaves on xa+9."""
        f = self.f
        keep_out = tuple(xa + o for o in range(-3, 10))
        gs = self.G(name + '.S', [s_src], x=xa, avoid=keep_out, z_min=z_min)
        gr = self.G(name + '.R', [r_src], x=xa + 6, loose=True, avoid=keep_out, z_min=z_min)
        z = max(gs.z1, gr.z1) + 6
        f.reserve_floor(xa - 3, xa + 9, z - 1, z + 3)
        return rs_latch(f, name, gs, gr, xa, z, ql=False)

    @build.red          # flip-flops keep the delayed torch (see build.red_torches)
    def _place_updown(self, name, x0):
        assert x0 % 3 == 2, (name, x0)
        c = updown3_place(self.f, name, x0, ZB - 40)
        for i in range(3):
            self.regs[f'{name}{i}'] = (x0 + PITCH * i + 4, Y, ZB - 40)
        return c

    @build.red          # flip-flops keep the delayed torch (see build.red_torches)
    def _place_counter(self, name, bits, x0):
        f, b = self.f, self.b
        assert x0 % 3 == 2, (name, x0)
        width = 18 * bits + 4
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

    @build.red          # flip-flops keep the delayed torch (see build.red_torches)
    def _place_fifo(self, name, stages, x0):
        f = self.f
        assert x0 % 3 == 0
        FW = 3 + 6 * stages + 6
        fz = ZB - 110
        f.reserve_floor(x0 - 4, x0 + FW, fz - 2, fz + 8 * stages + 6)
        dn = f.new_lane(f'{name}.D', x0 - 3, FAR, flow='north', loose=True)
        fifo = Fifo(f, name, dn, stages, x0, fz)
        return fifo, dn

    def _drive_placeholder(self, lane, src):
        """Feed a north-flowing placeholder from `src`: the landing row goes
        south of every tap already reading the lane."""
        zmin = (max(lane.taps) + 2) if lane.taps else None
        fx, fz = self.f.feedback(src, lane.x, z_min=zmin)
        assert not lane.taps or fz > max(lane.taps), (lane.name, fz, max(lane.taps))
        lane.taps.add(fz)
        lane.z0 = fz

    def _onehot(self, name, d0, d1, raw=False):
        d0.keep = True; d1.keep = True
        nd0 = self.G(f'{name}.n0', [d0], keep=True); nd1 = self.G(f'{name}.n1', [d1], keep=True)
        Nn = self.G(f'{name}.N', [d0, d1], keep=raw);   NL = self.G(f'{name}.N_L', [Nn], keep=True)
        E = self.G(f'{name}.E', [nd0, d1], keep=raw);  EL = self.G(f'{name}.E_L', [E], keep=True)
        S = self.G(f'{name}.S', [d0, nd1], keep=raw);  SL = self.G(f'{name}.S_L', [S], keep=True)
        Wg = self.G(f'{name}.W', [nd0, nd1], keep=raw); WL = self.G(f'{name}.W_L', [Wg], keep=True)
        return {'N_L': NL, 'E_L': EL, 'S_L': SL, 'W_L': WL, 'n0': nd0, 'n1': nd1, 'N': Nn, 'E': E, 'S': S, 'W': Wg}

    def _slot_for(self, inputs, prefer=None, z_min=None):
        """A free slot (not on any lane, not blocked for a lane starting at
        z_min) more than 4 cells from every input, searching east then west
        from `prefer` (default: just east of the inputs)."""
        f = self.f
        zc = ZB if z_min is None else z_min
        start = ((max(l.x for l in inputs) + 5 + 2) // 3) * 3 if prefer is None else prefer
        for step in (3, -3):
            x = start
            for _ in range(300):
                if (f.x_min <= x <= f.x_max and not f.by_slot.get(x) and not f.slot_blocked(x, zc, 'south')
                        and x not in self.reserved and all(abs(x - l.x) > 4 for l in inputs)
                        and all(abs(x - r) > 2 for r in self.reg_slots)):
                    return x
                x += step
        why = []
        for x in range(start - 60, start + 61, 3):
            r = []
            if not (f.x_min <= x <= f.x_max): r.append('range')
            if f.by_slot.get(x): r.append('lane:' + f.by_slot[x][0].name)
            if f.slot_blocked(x, ZB, 'south'): r.append('blocked')
            if x in self.reserved: r.append('reserved')
            near = [l.name for l in inputs if abs(x - l.x) <= 4]
            if near: r.append('near:' + ','.join(near))
            why.append((x, r))
        if build.PLUS and (f.x_min, f.x_max) != (-890, -48):
            # plus: rows cost nothing without decay, so a far slot is as good as a near one
            saved = (f.x_min, f.x_max)
            f.x_min, f.x_max = -890, -48
            try:
                return self._slot_for(inputs, prefer=prefer, z_min=z_min)
            finally:
                f.x_min, f.x_max = saved
        raise AssertionError(('no free slot near', start, why))

    # ── build ───────────────────────────────────────────────────────────
    # register D/W/Q columns (x, x+1, x+3) and their NOT-Q gate slot (x+9)
    REG_X = {'DIR0': -186, 'DIR1': -174,
             'FX0': -498, 'FX1': -486, 'FX2': -474, 'FY0': -84, 'FY1': -72, 'FY2': -60}
    HUB = (-378, -252)               # automatic placement range for the control hub
    YEAST = {-45, -39, -33, -12}     # hand-assigned slots at the east end of the Y band

    def build(self):
        b, f, w = self.b, self.f, self.w
        self.reg_slots = set()
        for x in self.REG_X.values():
            self.reg_slots.update((x, x + 1, x + 3, x + 9))
        self.reserved = set(self.YEAST) | set(range(-219, -191))    # east end + the two REQ latch spans
        self.at7 = {}                                                # restart: the counters' "at 7" detectors
        if PANEL:
            f.reserve_floor(BX - PORT_W - 2, BX + PSIZE + 4, -80, BZ + PSIZE + 8)
            self.ports = make_panel(b, BX, BZ)
        else:
            f.reserve_floor(BX - 8, BX + 8 * PW + 2, -80, BZ + 8 * PW + 3)
            self.ports = board(b, BX, BZ)
        # the platform's buttons: north-flowing lanes created now, so no gate takes their columns
        self.buttons = {}
        btn = {}
        ox0 = -378; ox0 += (-(ox0 + self.clock_n + 4)) % 3
        cx0 = ox0 + self.clock_n + 4
        seq_end = cx0 - 18 + sequencer_width(5, self.spacing) + 2      # the sequencer's floor ends here
        fb_x = ((seq_end + 3 + 2) // 3) * 3                       # DEADFB and PAUSE columns, east of the sequencer
        self.fb_x = fb_x
        bx0 = fb_x + 9
        assert bx0 + 9 < -219, ('buttons collide with the REQ latches', bx0)
        for i, d in enumerate(('N', 'E', 'S', 'W')):
            x = bx0 + 3 * i
            btn[d] = f.new_lane(f'B{d}', x, PZ - 1, flow='north', loose=True)   # driven from the deck (deck.py)
        self.btn = btn
        for l in btn.values(): l.keep = True

        def hub():
            # all east-side logic: X band, hub, Y band (plus: the empty columns west of the X band too,
            # the hub itself is full)
            f.x_min, f.x_max = (-620 if build.PLUS else -560), -48
        def anywhere():
            f.x_min, f.x_max = XMIN, -48

        # ---------------- clock band (the hub's floor) ----------------
        anywhere()
        ox = -378
        ox += (-(ox + self.clock_n + 4)) % 3
        with build.red_torches():
            cx, cz = oscillator(b, ox, ZB - 60, self.clock_n, active=self.clock_active)
        assert cx % 3 == 0, cx
        f.reserve_floor(ox - 3, cx + 2, ZB - 64, ZB - 57)
        clk = f.new_lane('CLK', cx, cz, loose=True)
        assert cx == cx0, (cx, cx0)
        # the clock gate sits at the WEST end of the band and the sequencer runs east from it;
        # the north-flowing feedback columns (DEAD, the start lever) sit just past the
        # sequencer's end so they never cross its floor
        self.deadfb = f.new_lane('DEADFB', fb_x, FAR, flow='north', loose=True)
        clk_l = self.G('CLK_L', [clk], x=cx - 12)
        self.clk = clk
        if build.PLUS:
            clk.keep = True            # the restart's clock-low hold reads CLK through its own inverter (CLK_L2)
        self.pause_lane = f.new_lane('PAUSE', fb_x + 3, PZ - 1, flow='north', loose=True)   # lever on the deck
        # the deck's torch drops onto these lanes (deck.py: x lane..lane+8, z PZ-1..PZ-9, levels 0..16) and the
        # death-lamp tap west of DEADFB: no gate rows there (the restart gates span the whole fabric)
        f.rects.append((fb_x - 8, fb_x + 3 + 12 + 10, PZ - 12, PZ + 2))
        self.pause = [(ox, Y, ZB - 63), None]
        # restart (plus): the reset-hold latch's Q, a placeholder until the latch exists; it
        # holds the clock gate shut until the reset has finished (_build_reset)
        # restart (plus): the reset signals are placeholders in the strip west of the fabric
        # until _build_reset_logic drives them at the very end (their rows then land south of
        # everything else instead of pushing the hub's rows down)
        self.rsth = self.rsth_l = self.rstx = self.rclk_l = self.hold = self.rstx_l = None
        if build.PLUS:
            # HUB slots (free this early): a gate reading a placeholder then has a short row;
            # only the drivers' landing rows, built last, cross from the west strip
            def ph(name, prefer):
                l = f.new_lane(name, self._slot_for([], prefer=prefer), FAR, flow='north', loose=True)
                l.keep = True
                return l
            # west of x = -260: their drivers' landing rows come last and far south, and a lane
            # extended down there must not cross the board's Y matrix (x -260 .. -48, z >= 0)
            self.rsth = ph('RSTH', -600)        # reset hold
            self.rsth_l = ph('RSTH_L', -600)    # NOT RSTH
            self.rstx = ph('RSTX', -600)        # RSTH stretched 8 ticks
            self.rclk_l = ph('RCLK_L', -600)    # NOT RCLK (the counters walk to their start values at RCLK pace)
            self.hold = ph('HOLD', -600)        # PAUSE | RSTH
            self.rstx_l = ph('RSTX_L', -600)    # NOT RSTX (the register forcing joins NOR(RSTX_L) = RSTX onto a lane)
            g = self.G('G', [clk_l, self.deadfb, self.hold], x=cx - 18)
            # the board's clear line source, made now so its row (west strip to the board) sits
            # north of everything; the lane runs south to the board (_board_clear)
            self.yclear = self.G('YCLEAR', [self.rsth_l], x=-27, keep=True)
        else:
            g = self.G('G', [clk_l, self.deadfb, self.pause_lane], x=cx - 18)
        seq_z = g.z0 + 8
        zh = seq_z + 10                                        # rows starting here clear the sequencer's floor
        f.reserve_floor(g.x - 1, g.x + sequencer_width(5, self.spacing) + 2, seq_z - 1, seq_z + 8)
        with build.red_torches():
            self.P = sequencer(f, g, g.x, seq_z, phases=5, spacing=list(self.spacing))
        P = self.P
        hub()
        PL = [self.G(f'P{k}_L', [p], keep=True) for k, p in zip('ABCDE', P)]
        self.PL = dict(zip('ABCDE', PL))
        A_L, B_L, C_L, D_L, E_L = PL
        if PANEL:
            # the wall's frame-buffer strobe: E (the board is complete: erase at B, set at D) or the
            # restart (the wall clears with the board): PRESENT = E | RSTH = NOR(NOR(E, RSTH)). Its
            # lane runs south past the panel to the wall.
            P[4].keep = True
            pre_l = self.G('PRESENT_L', [P[4], self.rsth])
            self.present = self.G('PRESENT', [pre_l], x=-24, keep=True)
        self.growp = f.new_lane('GROWP', self._slot_for([A_L]), FAR, flow='north', loose=True)
        self.growpl = f.new_lane('GROWPL', self._slot_for([A_L]), FAR, flow='north', loose=True)
        self.ncol = f.new_lane('NCOL', NCOL_X, self.zb, flow='north', loose=True)
        self.ncol.keep = True
        # the board pulse lines (active-low), placed EARLY so their rows sit far north of the
        # board: their lanes must reach the row taps at z >= 0. Every pulse is gated by DEAD
        # (DEADFB carries DEAD north), so the board freezes on death.
        per = self.G('PERASE', [B_L, self.growp, self.deadfb])                    # B & !GROW & !DEAD
        qset = self.G('QSET', [D_L, self.deadfb])                                 # D & !DEAD
        qchk = self.G('QCHECK', [C_L, self.deadfb])                               # C & !DEAD
        ylanes = {}
        ylanes['PSET'] = self.G('YPSET', [qset], x=-45, keep=True)
        ylanes['PCHECK'] = self.G('YPCHECK', [qchk], x=-39, keep=True)
        ylanes['PERASE'] = self.G('YPERASE', [per], x=-33, keep=True)

        # ---------------- control band: L, FIFOs, tail mux; RNG next to the FX registers ----------------
        anywhere()
        lq, l_in = self._place_counter('L', 4, -883)
        fifo0, f0d = self._place_fifo('F0', STAGES, -798)
        fifo1, f1d = self._place_fifo('F1', STAGES, -708)
        rox = -640
        rox += (1 - rox) % 3
        with build.red_torches():
            rx, rz = oscillator(b, rox, ZB - 100, 1)
        assert rx % 3 == 0, rx
        f.reserve_floor(rox - 3, rx + 2, ZB - 104, ZB - 97)
        self.pause.append((rox, Y, ZB - 103))
        rclk = f.new_lane('RCLK', rx, rz, loose=True)
        rng, rng_in = self._place_counter('RNG', 6, -616)
        self._drive_placeholder(rng_in, rclk)

        # ---------------- X band: HX, TX ----------------
        hx = self._place_updown('HX', -457)
        tx = self._place_updown('TX', -415)
        f.rects.append((-260, BX + PSIZE + 4, 0, BZ + PSIZE + 10) if PANEL else (-260, BX + 8 * PW, 0, BZ + 8 * PW + 3))      # the board rows
        # the X select lines climb off their lanes at z = -8 - 4k: keep rows off those cells
        # (with a cell of clearance) from the start, before any row lands next to them
        xlane_xs = ([c['Q'][i].x for c in (hx, tx) for i in range(3)] + [c['NQ'][i].x for c in (hx, tx) for i in range(3)]
                    + [self.REG_X[f'FX{i}'] + 3 for i in range(3)] + [self.REG_X[f'FX{i}'] + 9 for i in range(3)])
        for k in range(18):
            zl = -8 - 4 * k
            for xl in xlane_xs:
                f.rects.append((xl - 2, xl + 8, zl - 1, zl + 1))

        # ---------------- Y band: buttons, HY, TY ----------------
        hy = self._place_updown('HY', -160)
        ty = self._place_updown('TY', -124)

        # ---------------- length lines and the tail mux (matrix laid after build_lanes) ----------------
        # the NOT-L gates first: their rows must be NORTH of the mux region (their lanes flow
        # south through it), so they take the fabric's first rows and the mux starts below them
        lnq = [self.G(f'nL{i}', [lq[i]], x=lq[i].x + 6, keep=True) for i in range(4)]
        for l in lq: l.keep = True
        l_lines = {}
        for i in range(4):
            l_lines[f'Q{i}'] = lq[i].x; l_lines[f'NQ{i}'] = lnq[i].x
        fifo_q = [[l.x for l in fifo0.q], [l.x for l in fifo1.q]]
        td_x = [fifo_q[0][-1] + 9, fifo_q[1][-1] + 9]
        mux_ztop = max(l.z0 for l in lnq) + 4
        mux_zbus = row_z(mux_ztop, 0) + 6
        f.reserve_floor(min(l_lines.values()) - 2, td_x[1] + 5, mux_ztop - 1, mux_zbus + 2)
        for xl, zs in tap_cells(l_lines, fifo_q, mux_ztop).items():
            ln = next(l for l in list(lq) + list(lnq) + list(fifo0.q) + list(fifo1.q) if l.x == xl)
            ln.taps.update(zs); ln.extend(max(zs) + 1)
        for l in list(fifo0.q) + list(fifo1.q):
            l.keep = True
        td = [f.new_lane('TD0', td_x[0], mux_zbus, 'south', loose=True),
              f.new_lane('TD1', td_x[1], mux_zbus, 'south', loose=True)]
        self.mux = (l_lines, fifo_q, mux_ztop, td_x)

        # ---------------- request / direction registers, head direction ----------------
        hub()
        # REQ as two set/reset latches, so a press (a 20-tick level) never races a write window:
        # bit 0 (= E or W) is set by E/W and reset by N/S; bit 1 (= S or W) set by S/W, reset by N/E
        bl = list(btn.values())
        # restart (plus): RSTH forces REQ0 and REQ1 set = west (a button held during the reset
        # would fight it for 20 ticks, which nobody does)
        rst_in = [self.rsth] if build.PLUS else []
        nEW = self.G('nEW', [btn['E'], btn['W']] + rst_in, x=self._slot_for(bl, prefer=-252))
        nNS = self.G('nNS', [btn['N'], btn['S']], x=self._slot_for(bl, prefer=-252))
        nSW = self.G('nSW', [btn['S'], btn['W']] + rst_in, x=self._slot_for(bl, prefer=-252))
        nNE = self.G('nNE', [btn['N'], btn['E']], x=self._slot_for(bl, prefer=-252))
        rq0, _ = self._rs2('REQ0', nEW, nNS, -216)
        rq1, _ = self._rs2('REQ1', nSW, nNE, -201)
        rq0.keep = True; rq1.keep = True
        self.req_q = (rq0, rq1)
        zreg = ZB + 40
        # restart: the write is held open and the REQ latches are forced to "west" (nEW/nSW read RSTH), so DIR <- 1,1
        # (plus: the request decode's extra RSTH rows push DIR's D gates down, so DIR sits 20 further south (the PRESENT gate adds two more rows))
        dz = zreg + 30 + REG_DZ + (20 if build.PLUS else 0)
        d0 = self._reg('DIR0', rq0, P[1], dz, x=self.REG_X['DIR0'], init='hold')   # DIR <- REQ at B
        d1 = self._reg('DIR1', rq1, P[1], dz, x=self.REG_X['DIR1'], init='hold')
        self.DIR = (d0, d1)
        hd = self._onehot('HD', d0, d1, raw=build.PLUS)   # plus: the raw bits get copies south of the board (wall check)

        # ---------------- food registers: store NOT(rng), written at E if GROW ----------------
        hub()
        wf = self.G('WFOOD', [E_L, self.growpl], keep=True)                      # E & GROW
        anywhere()
        # the food registers store NOT(coordinate): the restart food is (0, 0), stored as ones,
        # which is the one value a direct register can be forced to (INIT_FOOD_PLUS; play2 and
        # write_snake_world start from it)
        fxn = [self._reg(f'FX{i}', rng[i], wf, ZB + 130 + REG_DZ, x=self.REG_X[f'FX{i}'], direct=True, init=1) for i in range(3)]
        fyn = [self._reg(f'FY{i}', rng[3 + i], wf, ZB + 130 + REG_DZ, x=self.REG_X[f'FY{i}'], direct=True, init=1) for i in range(3)]
        fx = [self.G(f'FXQ{i}', [fxn[i]], x=fxn[i].x + 6, keep=True) for i in range(3)]   # true polarity
        fy = [self.G(f'FYQ{i}', [fyn[i]], x=fyn[i].x + 6, keep=True) for i in range(3)]
        nfx, nfy = fxn, fyn
        for l in nfx + nfy: l.keep = True
        self.FX, self.FY = fx, fy
        self.food_inverted = True                       # FX/FY storage holds NOT(position)

        # ---------------- GROW = (head == food) and L < 12 ----------------
        def xnor_l(nm, a, na, c, nc):
            both = self.G(nm + '.ab', [na, nc])
            neither = self.G(nm + '.nn', [a, c])
            return self.G(nm + '.L', [both, neither], done=True)
        hub()
        eqx_l = [xnor_l(f'EQX{i}', hx['Q'][i], hx['NQ'][i], fx[i], nfx[i]) for i in range(3)]
        eqx = self.G('EQX', eqx_l); eqx_L = self.G('EQX_L', [eqx])
        hub()
        eqy_l = [xnor_l(f'EQY{i}', hy['Q'][i], hy['NQ'][i], fy[i], nfy[i]) for i in range(3)]
        eqy = self.G('EQY', eqy_l); eqy_L = self.G('EQY_L', [eqy])
        hub()
        at_max = self.G('LMAX', [lnq[2], lnq[3]])                                # L == 12
        grow = self.G('GROW', [eqx_L, eqy_L, at_max], keep=True)
        grow_l = self.G('GROW_L', [grow], keep=True)
        self.grow, self.grow_l = grow, grow_l

        # ---------------- head counters ----------------
        hub()
        sx_ = self.G('STEPX', [A_L, hd['n0']], keep=build.PLUS); self._walk_join('HX', hx, sx_); stepx_l = self.G('STEPX_L', [sx_], done=True)
        def near(c):                                        # T-gate slots just east of a counter (short feedback rows)
            return lambda ins: self._slot_for(ins, prefer=c['x0'] + 2 * PITCH + 12, z_min=max(i.z0 for i in ins) + 2)
        updown3_logic(f, 'HX', hx, stepx_l, hd['E_L'], t_slot=near(hx))
        hub()
        sy_ = self.G('STEPY', [A_L, d0], keep=build.PLUS);       self._walk_join('HY', hy, sy_); stepy_l = self.G('STEPY_L', [sy_], done=True)
        updown3_logic(f, 'HY', hy, stepy_l, hd['S_L'], t_slot=near(hy))

        # ---------------- length, FIFO (stores NOT direction), tail direction ----------------
        hub()
        lcount = self.G('LCNT', [C_L, grow_l])                 # L++ at C: GROW is stable from ~A+270; the new L reaches the tail mux ~C+380, long before the next B
        self._drive_placeholder(l_in, lcount)
        # the FIFO shifts at E and takes NOT(DIR) (DIR was latched from REQ at B, so the two
        # always agree): stage 0 = NOT(the next move's direction), settled long before the next B
        if not build.PLUS:                               # plus: driven from _build_reset_logic (the FIFOs shift "east" in)
            self._drive_placeholder(f0d, hd['n0'])
            self._drive_placeholder(f1d, hd['n1'])
        f.x_min, f.x_max = -890, -700
        e_ctl = self.G('E_ctl', [P[4]] + ([self.rsth] if build.PLUS else []), keep=True)   # restart: writes held open
        for k in range(STAGES):
            fifo0.drive_write(k, lambda x, join, k=k: self.G(f'F0W{k}', [e_ctl], x=x, join=join, loose=True))
            fifo1.drive_write(k, lambda x, join, k=k: self.G(f'F1W{k}', [e_ctl], x=x, join=join, loose=True))
        f.x_min, f.x_max = (-620 if build.PLUS else -420), -48   # the hub or east of it, not the control band
        tdd = self._onehot('TDD', td[0], td[1])
        td0h = self.G('TD0h', [tdd['n0']], keep=True)   # local copy of the raw bit for STEPTY
        hub()

        # ---------------- tail: step at E unless GROW (the erase pulse is PERASE at B) ----------------
        # the tail steps at E (the erase pulse at B must see settled tail select lines; the step's
        # new position reaches the lines ~E+300, long before the next B) unless GROW
        stx = self.G('STEPTX', [E_L, tdd['n0'], self.growp], keep=build.PLUS); self._walk_join('TX', tx, stx); stx_l = self.G('STEPTX_L', [stx], done=True)
        sty = self.G('STEPTY', [E_L, td0h, self.growp], keep=build.PLUS);       self._walk_join('TY', ty, sty); sty_l = self.G('STEPTY_L', [sty], done=True)
        hub()
        updown3_logic(f, 'TX', tx, stx_l, tdd['E_L'], t_slot=near(tx))
        hub()
        updown3_logic(f, 'TY', ty, sty_l, tdd['S_L'], t_slot=near(ty))

        # ---------------- wall check at A ----------------
        if build.PLUS: anywhere()                        # the hub is full once the restart placeholders sit in it
        else: hub()
        hx7 = self.G('HX7', [hx['NQ'][0], hx['NQ'][1], hx['NQ'][2]]); hx7_l = self.G('HX7_L', [hx7])
        hx0 = self.G('HX0', [hx['Q'][0], hx['Q'][1], hx['Q'][2]]);    hx0_l = self.G('HX0_L', [hx0])
        if not build.PLUS: hub()
        hy7 = self.G('HY7', [hy['NQ'][0], hy['NQ'][1], hy['NQ'][2]]); hy7_l = self.G('HY7_L', [hy7])
        hy0 = self.G('HY0', [hy['Q'][0], hy['Q'][1], hy['Q'][2]]);    hy0_l = self.G('HY0_L', [hy0])
        if not build.PLUS: hub()
        if build.PLUS:
            # plus: south of the board (rows at z >= ZSOUTH, where every column is free again). The
            # head-direction bits and the board's collision line live inside the Y matrix (x -260..-48),
            # whose columns cannot run past z = 0: they get inverted copies on columns west of it.
            ZS = self.ZSOUTH
            ZC = -60                                     # the copies' rows: the band just north of the board is empty
            hdw = {}
            for k in 'EWSN':
                x = self._slot_west_of([hd[k]], -262, ZC)
                hdw[k] = self.G(f'HDS.{k}_L', [hd[k]], x=x, keep=True, z_min=ZC, z_max=-2)
            wall = self.G('WALL', [hdw['E'], hx7_l, A_L], z_min=ZS, keep=True)
            for k, (hl, el) in enumerate((('W', hx0_l), ('S', hy7_l), ('N', hy0_l)), 2):   # joins land SOUTH of the lane's start
                self.G(f'WALL{k}', [hdw[hl], el, A_L], x=wall.x, join=wall, z_min=wall.z0 + 2)
            # ---------------- death ----------------
            ncl = self.G('NCOL_L', [self.ncol], x=self._slot_west_of([self.ncol], -262, ZC), keep=True, z_min=ZC, z_max=-2)
            nc2 = self.G('NCOL2', [ncl], z_min=ZS)
            nc = self.G('NC', [nc2, wall], z_min=ZS)
            dead_q, _ = self._rs2('DEAD', nc, self.rsth_l, self._south_latch_x(XMIN + 18), z_min=ZS)   # restart resets DEAD
        else:
            # the slots under the sequencer's floor are free for gates whose rows start south of it
            wins = [hd['E_L'], hd['W_L'], hd['S_L'], hd['N_L'], hx7_l, hx0_l, hy7_l, hy0_l, A_L]
            wallx = self._slot_for(wins, prefer=g.x + 3, z_min=zh)
            wall = self.G('WALL', [hd['E_L'], hx7_l, A_L], x=wallx, z_min=zh, keep=True)
            self.G('WALL2', [hd['W_L'], hx0_l, A_L], x=wall.x, join=wall)
            self.G('WALL3', [hd['S_L'], hy7_l, A_L], x=wall.x, join=wall)
            self.G('WALL4', [hd['N_L'], hy0_l, A_L], x=wall.x, join=wall)

            # ---------------- death ----------------
            ncx = self._slot_for([self.ncol, wall], prefer=g.x + 3, z_min=zh)
            nc = self.G('NC', [self.ncol, wall], x=ncx, z_min=zh)
            xa = self._slot_for([nc], prefer=g.x + 3, z_min=zh)
            while any(f.by_slot.get(xa + o) or f.slot_blocked(xa + o, zh, 'south') or (xa + o) in self.reserved
                      for o in range(-3, 10)):
                xa += 3
            dead_q, _ = self._rs_at('DEAD', nc, xa, z_min=zh)
        self.dead_q = dead_q
        b.lamp(dead_q.x, Y - 1, dead_q.z0)
        dead_q.keep = True

        # ---------------- placeholders driven last ----------------
        hub()
        self._drive_placeholder(self.growp, self.grow)
        self._drive_placeholder(self.growpl, self.grow_l)
        self._drive_placeholder(self.deadfb, dead_q)
        if build.PLUS:
            self._build_reset_logic(lq, lnq, rng[0], l_in, hd, d0, d1, f0d, f1d)
        # deck.py taps DEADFB at the platform row (PZ) for the death lamp: the lane must
        # pass it. The vanilla layout's DEAD latch sits south of PZ so it does; the plus
        # layouts (shorter sequencer) put it north, so the lane gets a stub south of its
        # source down to the platform row — no repeaters on a plus lane, so the stub
        # carries the signal (measured 2026-09-11: the tap sat on air, lamp never lit).
        if build.PLUS and self.deadfb.z0 < PZ + 2:
            self.deadfb.z0 = PZ + 2
        # DEADFB runs north past the platform on its way to the clock gate: deck.py taps the block
        # under its cell at the platform row for the death lamp
        anywhere()

        # ---------------- select lines ----------------
        ysel = {}
        for gname, c in (('HY', hy), ('TY', ty)):
            for i in range(3):
                ysel[f'{gname}Q{i}'] = (c['Q'][i], False)
                ysel[f'{gname}NQ{i}'] = (c['NQ'][i], True)
        for i in range(3):
            ysel[f'FYQ{i}'] = (fy[i], True)           # FYQ gate lanes at reg+9: tap east (x+10..x+13)
            ysel[f'FYNQ{i}'] = (nfy[i], True)         # the register's own lane (reg+3): tap east too (x+4..x+7)
        for k in ('PSET', 'PCHECK', 'PERASE'):
            ysel[k] = (ylanes[k], True)
        rows = []
        if PANEL:
            rz = PROW_Z; pitch = PPITCH
            row_side = lambda z: +1                                       # panel.py: every tap south of its row
        else:
            rz = {'HYS': ROW_Z[0], 'HYC': ROW_Z[1], 'TYE': ROW_Z[2], 'FY': ROW_Z[3]}; pitch = PW
            row_side = lambda z: ROW_SIDE[(z - BZ) % PW]
        for j in range(N):
            zj = BZ + pitch * j
            rows.append((zj + rz['HYS'], ['HY' + n for n in bits_for(j)] + ['PSET']))
            rows.append((zj + rz['HYC'], ['HY' + n for n in bits_for(j)] + ['PCHECK']))
            rows.append((zj + rz['TYE'], ['TY' + n for n in bits_for(j)] + ['PERASE']))
            rows.append((zj + rz['FY'], ['FY' + n for n in bits_for(j)]))
        for z, names in rows:
            side = row_side(z)
            for nme in names:
                ln, east = ysel[nme]
                assert ln.flow == 'south' and ln.z0 < -2, ('Y line starts too far south', nme, ln.x, ln.z0)
                ln.taps.add(z + side)
                ln.extend(z + side + 1)
        xsrc = {'HX': (hx['Q'], hx['NQ']), 'TX': (tx['Q'], tx['NQ']), 'FX': (fx, nfx)}
        groups = []
        zline = -8
        self.xlines = {}
        for gname in ('HX', 'TX', 'FX'):
            qs, nqs = xsrc[gname]
            lines = {}
            for n in BITS:
                bit = int(n[-1])
                ln = qs[bit] if n.startswith('Q') else nqs[bit]
                ln.keep = True
                ln.taps.add(zline); ln.extend(zline)
                lines[n] = zline
                self.xlines[(gname, n)] = (ln, zline)
                zline -= 4
            cols = [(self.ports[gname][i][0], bits_for(i)) for i in range(N)]
            groups.append((gname, lines, cols))

        if build.PLUS:
            self.yclear.extend(self.zclear)  # the CLEAR source runs down to the board (its ramp is laid in _board_clear / _panel_clear)
        if PANEL:
            self.present.extend(self.ports['PRESENT'][2])            # to the wall's PRESENT staircase (laid in _panel_present)
        f.build_lanes()
        near = [r for r in f.rects if r[1] >= -560 and r[3] < 0 and r[3] != r[2] - 0 or False]
        rows_z = [r for r in f.rects if r[3] < 0 and r[1] >= -260]      # only rows that reach the board's x range
        assert max(r[3] for r in rows_z) <= -2, ('fabric rows reach the board', max(rows_z, key=lambda r: r[3]))
        assert not b.collisions, ('overwritten blocks', b.collisions[:20])

        tailmux(b, *self.mux)
        for (gname, n), (ln, zl) in self.xlines.items():
            sx = xvia_up(b, ln.x, zl)
            xline(b, zl, sx, BX - 7)
        if PANEL:
            xmatrix(b, BX, BZ, groups, x_last=self.ports['x_last'], z_bot=BZ - PORT_N - 1)
        else:
            xmatrix(b, BX, BZ, groups)
        if PANEL:
            self._panel_clear(b)
            self._panel_present(b)
        elif build.PLUS:
            self._board_clear(b)
        for z, names in rows:
            side = row_side(z)
            joins = set()
            for nme in names:
                ln, east = ysel[nme]
                ytap(b, ln.x, z, side, east=east)
                joins.add(ytap_join(ln.x, east))
            yrow(b, z, min(joins), (BX - PORT_W - 1) if PANEL else (BX - 1), joins)
        if PANEL:
            self._panel_col(b)
        else:
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
            b.wire(NCOL_X, L0, zb)
        assert not b.collisions, ('overwritten blocks (hand parts)', b.collisions[:20])

        # ---------------- the screen and the viewing deck ----------------
        if not PANEL:
            self.screen = screen(b)          # the panel IS the display; the lamp wall mirrors the lamp board only
        info = deck(b, {d: btn[d].x for d in 'NESW'} | {'PAUSE': self.pause_lane.x}, (fb_x, PZ),
                    origin=DECK_ORIGIN if PANEL else (0, 0))
        self.buttons = info['buttons']
        self.pause[1] = info['lever']
        self.dead_lamp = info['lamp']
        self.spawn = info['spawn']
        assert not b.collisions, ('overwritten blocks (screen/deck)', b.collisions[:20])
        self.stats = f.stats()
        xs = [l.x for l in f.lanes.values()]
        rows_z = [r for r in f.rects if r[3] < 0]
        print('stats', self.stats, 'blocks', len(w.blocks), 'x', min(xs), max(xs),
              'rows z', min(r[2] for r in rows_z), max(r[3] for r in rows_z))


if __name__ == '__main__':
    m = Machine3()
