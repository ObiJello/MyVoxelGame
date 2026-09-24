"""Structured place-and-route on the two-layer scheme (pcb.py).

Signals are LANES: floor wires running along z at an x slot, flowing south
(source at the north end) or north. Gates sit on the upper layer; each input
is a ROW that taps a lane through a via and runs east to the gate. A gate's
output drops through a via onto a lane directly under the gate.

Rows are packed in two dimensions: a gate reserves the rectangle spanned by
its input rows (from the westernmost tapped lane to two cells east of the
gate) over the z of its rows, and the allocator picks the lowest z at which
that rectangle, with one cell of clearance, is free. Rows of gates in
different x regions therefore share z, which is what keeps the machine's
height bounded by its deepest column of logic rather than its gate count.

Slots: lanes and gates live on x slots. Lanes at the same slot are allowed
when their z ranges are disjoint (checked as taps are added), so a slot is
reused once every lane on it has ended. Rules kept:
  * a lane slot has x+1, x+2 free on the floor (via ramps) — slots >= 3 apart
    unless flagged loose (macro ports whose z ranges are known disjoint)
  * a lane cell carrying a via is plain wire (no repeater)
  * a row never passes over another via at its z (rectangles do not overlap)
  * a gate at slot x is east of every lane it reads (>= 5 cells)
"""
import build
from build import Builder, Y
from cells import run_wire
from pcb import Pcb, UP, MID
from redsim import DIRS


class Lane:
    def __init__(self, name, x, z0, flow):
        self.name, self.x, self.z0, self.flow = name, x, z0, flow   # flow: 'south' | 'north'
        self.z1 = z0
        self.taps = set()          # z of via cells
        self.built = False
        self.closed = None         # rows beyond this z (in flow direction) are forbidden
        self.done = False          # no more consumers will tap it (slot may be reused)
        self.merge_with = None     # a lane on the same slot carrying the same net
        self.keep = False          # has several consumers: never auto-closed

    def extend(self, z):
        if self.flow == 'south':
            assert self.closed is None or z < self.closed, (self.name, z, self.closed)
            self.z1 = max(self.z1, z)
        else:
            assert self.closed is None or z > self.closed, (self.name, z, self.closed)
            self.z1 = min(self.z1, z)

    def zrange(self):
        return (min(self.z0, self.z1), max(self.z0, self.z1))


class Fabric:
    def __init__(self, b, x0=0, z0=0, row_pitch=2):
        self.b = b
        self.p = Pcb(b)
        self.lanes = {}
        self.by_slot = {}
        self.x0 = x0
        self.z_base = z0
        self.row_pitch = row_pitch
        self.rects = []            # (x0, x1, z0, z1) reserved on the upper layer
        self.floor = []            # (x0, x1, z0, z1) floor macros: no auto slots inside
        self.next_row_z = z0       # kept for callers that want a z past everything
        self.gate_count = 0
        self.x_min = -10**9        # automatic placement stays inside [x_min, x_max]
        self.x_max = 10**9

    def reserve_floor(self, x0, x1, z0, z1):
        """A floor macro lives here: rows stay off it and no automatically
        chosen slot lands on it."""
        self.rects.append((x0, x1, z0, z1))
        self.floor.append((x0, x1, z0, z1))

    def slot_blocked(self, x, z=None, flow='south'):
        """A slot is blocked by a floor macro that a lane starting at z on
        it would cross: with z unknown any macro on the slot counts; with z,
        only macros at or beyond z in the flow direction (a gate south of a
        macro is fine on that slot, its lane never reaches the macro)."""
        for (a, b_, c, d) in self.floor:
            if a - 1 <= x <= b_ + 1:
                if z is None:
                    return True
                if flow == 'south' and d >= z - 1:
                    return True
                if flow == 'north' and c <= z + 1:
                    return True
        # a lane within two cells (an off-grid macro port) conflicts if it
        # is alive at z or starts south of it; one that ended north is fine
        for dd in (-2, -1, 1, 2):
            for l in self.by_slot.get(x + dd, []):
                if z is None or not l.done:
                    return True                  # still growing: alive for all z beyond it
                a, b_ = l.zrange()
                if b_ >= z - 1:
                    return True
        return False

    # ── lanes ───────────────────────────────────────────────────────────
    def new_lane(self, name, x, z0, flow='south', loose=False, merge_with=None, keep=None):
        """`loose` skips the 3-slot separation check for a lane whose z range
        is known to be disjoint from its neighbours (macro ports).
        `merge_with` names a lane on the same slot carrying the SAME net (a
        feedback landing that feeds consumers both north and south)."""
        assert name not in self.lanes, name
        if merge_with is not None:
            merge_with = self.lanes[merge_with] if isinstance(merge_with, str) else merge_with
        if not loose:
            assert not self.slot_blocked(x, z0, flow), (name, x, z0, 'would cross a floor macro or a live lane')
            for l in self.lanes.values():
                if l.x != x and abs(l.x - x) < 3:
                    a, b_ = l.zrange()
                    assert l.done and b_ < z0 - 1, (name, x, z0, 'beside live', l.name, l.x, l.zrange())
        else:
            # a macro port may sit off the 3-grid, but a lane within two cells
            # of it must not share any z (they would touch or ramp into it)
            for d in (-2, -1, 1, 2):
                for l in self.by_slot.get(x + d, []):
                    a, b_ = l.zrange()
                    assert not (a - 1 <= z0 <= b_ + 1), (name, x, z0, 'beside', l.name, l.x, l.zrange())
        for l in self.by_slot.get(x, []):
            if l is merge_with:
                continue
            # same slot: the new lane starts where no other lane on the slot is
            a, b_ = l.zrange()
            assert not (a - 1 <= z0 <= b_ + 1), (name, x, z0, l.name, l.zrange())
        ln = Lane(name, x, z0, flow)
        ln.merge_with = merge_with
        ln.keep = loose if keep is None else keep      # macro ports default to keep
        self.lanes[name] = ln
        self.by_slot.setdefault(x, []).append(ln)
        return ln

    def _check_slot(self, ln):
        a, b_ = ln.zrange()
        for l in self.by_slot.get(ln.x, []):
            if l is ln or l is ln.merge_with or ln is l.merge_with:
                continue
            c, d = l.zrange()
            assert b_ < c - 1 or a > d + 1, ('lanes overlap on slot', ln.name, (a, b_), l.name, (c, d))

    def slot_free_below(self, x, z):
        """True when every lane on slot x is DONE (no consumer will tap it
        again) and ends north of z, so a gate whose lane starts at z can use
        the slot."""
        for l in self.by_slot.get(x, []):
            a, b_ = l.zrange()
            if not l.done or b_ >= z - 1:
                return False
        return True

    def close(self, *lanes):
        """Declare that no further gate will read these lanes."""
        for l in lanes:
            ln = self.lanes[l] if isinstance(l, str) else l
            ln.done = True
            ln.closed = ln.z1 + 2 if ln.flow == 'south' else ln.z1 - 2

    # ── rows ────────────────────────────────────────────────────────────
    def _free(self, x0, x1, z0, z1):
        for (a, b_, c, d) in self.rects:
            if x0 - 1 <= b_ and x1 + 1 >= a and z0 - 1 <= d and z1 + 1 >= c:
                return False
        return True

    def alloc_rows(self, x0, x1, n, z_min=None, z_max=None):
        """n consecutive rows (pitch 2) whose rectangle [x0, x1] is free,
        with the first row at z >= z_min (and the last <= z_max if given)."""
        z = self.z_base if z_min is None else z_min
        while True:
            z1 = z + self.row_pitch * (n - 1)
            assert z_max is None or z1 <= z_max, ('no rows fit', x0, x1, n, z_min, z_max)
            if self._free(x0, x1, z, z1):
                self.rects.append((x0, x1, z, z1))
                self.next_row_z = max(self.next_row_z, z1 + self.row_pitch)
                return [z + self.row_pitch * i for i in range(n)]
            z += self.row_pitch

    def alloc_row(self):
        """A full-width row (legacy callers)."""
        return self.alloc_rows(-10**6, 10**6, 1)[0]

    def tap_row(self, lane, z, x_end):
        """Row at z from `lane` to x_end (inclusive), east if x_end is east of
        the lane, west otherwise. Registers the via."""
        lane.taps.add(z)
        lane.extend(z)
        self._check_slot(lane)
        if x_end > lane.x:
            start = self.p.via_up(lane.x, z)          # (lane.x+4, z), repeater at +3
            if x_end >= start[0]:
                self.p.row(start[0], x_end, z)
        else:
            start = self.p.via_up_west(lane.x, z)     # (lane.x-4, z)
            if x_end <= start[0]:
                self.p.row(start[0], x_end, z)
        return start

    # ── gates ───────────────────────────────────────────────────────────
    def gate(self, name, inputs, x=None, out_flow='south', join=None, loose=False, z_min=None,
             done=False, min_x=None, close=(), side=None, keep=False, avoid=(), x_max=None, z_max=None):
        """NOR gate reading up to 3 lanes. `side` is where the gate stands
        relative to its inputs: 'east' (inputs to the west, torch east) or
        'west' (mirrored). With x=None the slot is chosen automatically on
        whichever side is closer; the first input feeds the side face, the
        others turn into the north/south faces from wherever they are.
        Returns the output Lane (or the joined lane)."""
        lanes = [self.lanes[i] if isinstance(i, str) else i for i in inputs]
        assert 1 <= len(lanes) <= 3, name
        xin_max = max(l.x for l in lanes); xin_min = min(l.x for l in lanes)
        n = len(lanes)
        auto = x is None
        if auto:
            cand = []
            if side in (None, 'east'):
                xe = ((xin_max + 5 + 2) // 3) * 3
                if min_x is not None:
                    xe = max(xe, ((min_x + 2) // 3) * 3)
                xe = max(xe, ((self.x_min + 2) // 3) * 3)
                cand.append(('east', xe, +3))
            if side in (None, 'west'):
                xw = -((-(xin_min - 5) + 2) // 3) * 3          # first multiple of 3 <= xin_min-5
                xw = min(xw, ((self.x_max if x_max is None else min(self.x_max, x_max)) // 3) * 3)
                cand.append(('west', xw, -3))
            # the side-face input is lanes[0]; the candidates above clear every
            # input, which keeps rows short
        else:
            # explicit slot: the side-face input is the input farthest from
            # the slot (any input more than 4 cells away will do); the rest
            # turn into the north/south faces from either side
            first = max(lanes, key=lambda l: abs(x - l.x))
            assert abs(x - first.x) > 4, (name, x, [(l.name, l.x) for l in lanes])
            lanes = [first] + [l for l in lanes if l is not first]
            if side is None:
                side = 'east' if x > first.x else 'west'
            cand = [(side, x, 0)]
        # try candidates: slot free (not blocked, lanes done north of the rows)
        zlo = self.z_base if z_min is None else z_min
        zhi = z_max
        for l in lanes:
            if l.flow == 'south':
                zlo = max(zlo, l.z0 + 2)
            else:
                zhi = l.z0 - 2 if zhi is None else min(zhi, l.z0 - 2)
        chosen = None
        best = None
        why = []
        for sd, x0, step in cand:
            xx = x0
            lim_hi = self.x_max if x_max is None else min(self.x_max, x_max)
            for _ in range(600):
                if step and not (self.x_min <= xx <= lim_hi):
                    why.append((sd, xx, 'out of range')); break
                if step and any(abs(xx - a_) <= 4 for a_ in avoid):
                    xx += step; continue
                if step and self.slot_blocked(xx):
                    # conservatively blocked (a macro on the slot); allow it
                    # when the macro lies north of every row we could use
                    if step and self.slot_blocked(xx, zlo, out_flow):
                        why.append((sd, xx, 'blocked at zlo', zlo)); xx += step; continue
                rx0 = min(xin_min, xx - 2); rx1 = max(xin_max, xx + 2)
                try:
                    rows = self.alloc_rows(rx0, rx1, n, zlo, zhi)
                except AssertionError as e:
                    raise AssertionError((name, [(l.name, l.x, l.flow, l.z0) for l in lanes], e.args)) from None
                z = rows[1] if n == 3 else rows[0]
                ok = (join is not None or loose or self.slot_free_below(xx, z)) and \
                     (loose or not self.slot_blocked(xx, z, out_flow))
                if ok:
                    dist = abs(xx - (xin_min + xin_max) / 2)
                    if best is None or dist < best:
                        if chosen is not None:
                            self.rects.remove(chosen[3])
                        best = dist; chosen = (sd, xx, rows, self.rects[-1])
                    else:
                        self.rects.pop()
                    break
                self.rects.pop()
                why.append((sd, xx, 'rows', rows, 'slot free', self.slot_free_below(xx, z), 'blocked', self.slot_blocked(xx, z, out_flow)))
                if not step:
                    raise AssertionError((name, 'slot', xx, 'busy at rows', rows))
                xx += step
            else:
                pass                      # no slot on this side within the limits
        assert chosen is not None, (name, [(l.name, l.x, l.z0) for l in lanes], 'zlo', zlo, 'zhi', zhi, why[:6], why[-6:])
        side, x, rows, _ = chosen
        # the side-face input must be on the input side; the others may come
        # from either side (their rows turn into the north/south faces)
        assert (x > lanes[0].x + 4) if side == 'east' else (x < lanes[0].x - 4), (name, side, x, lanes[0].x)
        assert all(abs(x - l.x) > 4 for l in lanes), (name, x, [(l.name, l.x) for l in lanes])
        p = self.p
        if n == 3:
            zn, zw, zsx = rows
        elif n == 2:
            zw, zsx = rows; zn = None
        else:
            zw = rows[0]; zn = zsx = None
        z = zw
        out_dir = 'east' if side == 'east' else 'west'
        face_x = x - 1 if side == 'east' else x + 1
        (tx, tz), faces = p.gate(x, z, out_dir)
        self.tap_row(lanes[0], z, face_x)
        if zsx is not None:
            self.tap_row(lanes[1], zsx, x)
            for zz in range(zsx - 1, z, -1):
                p.upper_wire(x, zz)
        if zn is not None:
            self.tap_row(lanes[2], zn, x)
            for zz in range(zn + 1, z):
                p.upper_wire(x, zz)
        p.torch_out((tx, tz), out_dir, 1)
        if side == 'east':
            p.via_down(x, z)
        else:
            p.via_down_west(x, z)
        self.gate_count += 1
        # a lane with a single consumer is finished once it has been read;
        # lanes with several consumers are created with keep=True
        for l in lanes:
            if not l.keep:
                self.close(l)
        if done:
            self.close(*lanes)
        if close:
            self.close(*close)
        if join is not None:
            ln = self.lanes[join] if isinstance(join, str) else join
            assert ln.x == x, (name, ln.name, ln.x, x)
            assert ln.flow != 'south' or z > ln.z0, (name, 'join rows north of the lane start (pass z_min=lane.z0+2)', z, ln.z0)
            ln.taps.add(z); ln.extend(z)
            self._check_slot(ln)
            return ln
        return self.new_lane(name, x, z, out_flow, loose=loose, keep=keep)

    def feedback(self, lane, x_target, z_min=None):
        """Carry `lane` to a floor cell at slot x_target on ONE row: a via up
        on the side facing the target, the row, and a via down at the target
        arriving from the lane's side. Returns the floor cell (x_target, z);
        the caller places the wire there (or starts a lane at it). `z_min`
        forces the row south of a given z (a north-flowing placeholder must
        land south of every tap that reads it)."""
        assert abs(x_target - lane.x) > 4, (lane.name, lane.x, x_target)
        zlo = lane.z0 + 2 if lane.flow == 'south' else None
        if z_min is not None:
            zlo = z_min if zlo is None else max(zlo, z_min)
        zhi = None if lane.flow == 'south' else lane.z0 - 2
        p = self.p
        if x_target < lane.x:
            (za,) = self.alloc_rows(x_target, lane.x, 1, zlo, zhi)
            lane.taps.add(za); lane.extend(za); self._check_slot(lane)
            start = p.via_up_west(lane.x, za)               # (lane.x-4, za)
            p.row(start[0], x_target + 3, za)
            p.via_down(x_target, za)                         # arrival from the east at x+2
        else:
            (za,) = self.alloc_rows(lane.x, x_target, 1, zlo, zhi)
            lane.taps.add(za); lane.extend(za); self._check_slot(lane)
            start = p.via_up(lane.x, za)                     # (lane.x+4, za)
            p.row(start[0], x_target - 3, za)
            p.via_down_west(x_target, za)                    # arrival from the west at x-2
        return (x_target, za)

    # ── finish ──────────────────────────────────────────────────────────
    def build_lanes(self, rep_every=12):
        """Lay every lane. A repeater goes on the second cell (the first may
        be a weak macro output) and then at most `rep_every` cells apart, so
        every cell keeps at least 5 power and a via ramp off it still reaches
        its own repeater. Tap cells never get a repeater."""
        b = self.b
        for ln in self.lanes.values():
            if ln.built:
                continue
            self._check_slot(ln)
            z0, z1 = ln.z0, ln.z1
            step = 1 if z1 >= z0 else -1
            back = 'north' if step > 0 else 'south'          # repeater faces its input
            cells = list(range(z0, z1 + step, step))
            since = rep_every                                # force one early
            for i, z in enumerate(cells):
                is_tap = z in ln.taps
                if i == 0 or i == len(cells) - 1 or is_tap:
                    b.wire(ln.x, Y, z); since += 1
                elif since >= rep_every and not build.PLUS:
                    b.repeater(ln.x, Y, z, back); since = 0
                else:
                    b.wire(ln.x, Y, z); since += 1
            ln.built = True

    def stats(self):
        xs = [l.x for l in self.lanes.values()]
        zs = [z for l in self.lanes.values() for z in l.zrange()]
        return {'gates': self.gate_count, 'lanes': len(self.lanes),
                'x': (min(xs), max(xs)) if xs else None, 'z': (min(zs), max(zs)) if zs else None}
