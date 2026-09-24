"""Steady-state redstone simulator for the world generator.

A small, faithful port of the vanilla signal rules (SignalGetter, RedstoneWireBlock
+ DefaultRedstoneWireEvaluator, DiodeBlock/RepeaterBlock/ComparatorBlock,
RedstoneTorchBlock/RedstoneWallTorchBlock, LeverBlock, RedstoneLampBlock) for the
handful of blocks the computer uses. It only answers "what does the circuit settle
to", which is what a combinational design needs verified, and it produces the exact
block states the save file is written with.

Coordinates: x east, y up, z south (north = -z), as in Minecraft.
"""
from collections import deque

DIRS = {
    'down': (0, -1, 0), 'up': (0, 1, 0),
    'north': (0, 0, -1), 'south': (0, 0, 1),
    'west': (-1, 0, 0), 'east': (1, 0, 0),
}
OPP = {'down': 'up', 'up': 'down', 'north': 'south', 'south': 'north', 'west': 'east', 'east': 'west'}
HORIZ = ['north', 'east', 'south', 'west']
AXIS = {'north': 'z', 'south': 'z', 'east': 'x', 'west': 'x', 'up': 'y', 'down': 'y'}
CW = {'north': 'east', 'east': 'south', 'south': 'west', 'west': 'north'}
CCW = {v: k for k, v in CW.items()}
N6 = ('down', 'up', 'north', 'south', 'west', 'east')


def add(p, d):
    o = DIRS[d]
    return (p[0] + o[0], p[1] + o[1], p[2] + o[2])


DISPLAY_OPS = ['none', 'set_r', 'set_g', 'set_b', 'clear_r', 'clear_g', 'clear_b',
               'show_r', 'show_g', 'show_b', 'check_r', 'check_g', 'check_b', 'latch']   # the engine's op_nw / op_se value order


class Block:
    __slots__ = ('kind', 'facing', 'lit', 'powered', 'power', 'mode', 'output', 'delay', 'face', 'name', 'sides', 'instant',
                 'rgb', 'op_nw', 'op_se')

    def __init__(self, kind, **kw):
        self.kind = kind
        self.facing = kw.get('facing')
        self.lit = kw.get('lit', False)
        self.powered = kw.get('powered', False)
        self.power = kw.get('power', 0)
        self.mode = kw.get('mode', 'compare')
        self.output = kw.get('output', 0)
        self.delay = kw.get('delay', 1)
        self.instant = kw.get('instant', False)    # blue torch (redstone_plus): flips with no delay
        self.face = kw.get('face', 'floor')
        self.name = kw.get('name')          # registry name for 'solid' blocks
        self.rgb = kw.get('rgb', 0)         # display block: colour bits (1 red, 2 green, 4 blue), shared by a vertical run
        self.op_nw = kw.get('op_nw', 'none')   # display block: operation on its north&west line pair (DISPLAY_OPS)
        self.op_se = kw.get('op_se', 'none')   # ... and on south&east
        self.sides = None                   # wire: dict side -> 'none'|'side'|'up'

    def copy(self):
        b = Block(self.kind)
        for s in Block.__slots__:
            setattr(b, s, getattr(self, s))
        return b


class World:
    def __init__(self):
        self.blocks = {}
        self.wires_muted = False
        self.no_decay = False       # redstone_plus: wires carry their full signal any distance

    # ── construction ──────────────────────────────────────────────────
    def set(self, pos, block):
        if block is None:
            self.blocks.pop(pos, None)
        else:
            self.blocks[pos] = block

    def get(self, pos):
        return self.blocks.get(pos)

    def kind(self, pos):
        b = self.blocks.get(pos)
        return b.kind if b else 'air'

    # ── block classification ──────────────────────────────────────────
    def is_conductor(self, pos):
        return self.kind(pos) in ('solid', 'lamp')

    def is_signal_source(self, pos):
        return self.kind(pos) in ('wire', 'repeater', 'comparator', 'torch', 'wtorch', 'lever', 'button', 'display')

    def is_diode(self, pos):
        return self.kind(pos) in ('repeater', 'comparator')

    def face_sturdy_up(self, pos):
        return self.kind(pos) in ('solid', 'lamp', 'glow', 'display')

    def wire_can_survive_on(self, pos):
        return self.face_sturdy_up(pos)

    # ── per-block signal (BlockBehaviour.getSignal / getDirectSignal) ─
    def block_signal(self, pos, direction):
        """state.getSignal(level, pos, direction): `direction` is the direction the
        asking block looked in to reach `pos`."""
        b = self.blocks.get(pos)
        if b is None:
            return 0
        k = b.kind
        if k == 'wire':
            if self.wires_muted or direction == 'down':
                return 0
            if b.power == 0:
                return 0
            if direction == 'up':
                return b.power
            return b.power if self.wire_sides(pos)[OPP[direction]] != 'none' else 0
        if k == 'repeater':
            return 15 if (b.powered and b.facing == direction) else 0
        if k == 'comparator':
            return b.output if b.facing == direction else 0
        if k == 'torch':
            return 15 if (b.lit and direction != 'up') else 0
        if k == 'wtorch':
            return 15 if (b.lit and b.facing != direction) else 0
        if k in ('lever', 'button'):
            return 15 if b.powered else 0
        if k == 'display':                  # the run's top block powers what sits on it (the dust above asks 'down')
            return 15 if (b.powered and direction == 'down') else 0
        return 0

    def lever_connected_direction(self, b):
        # FaceAttachedHorizontalDirectionalBlock.getConnectedDirection
        if b.face == 'ceiling':
            return 'down'
        if b.face == 'floor':
            return 'up'
        return b.facing

    def block_direct_signal(self, pos, direction):
        b = self.blocks.get(pos)
        if b is None:
            return 0
        k = b.kind
        if k == 'wire':
            return 0 if self.wires_muted else self.block_signal(pos, direction)
        if k in ('repeater', 'comparator'):
            return self.block_signal(pos, direction)
        if k == 'torch':
            return self.block_signal(pos, direction) if direction == 'down' else 0
        if k == 'wtorch':
            # RedstoneWallTorchBlock inherits RedstoneTorchBlock.getDirectSignal
            return self.block_signal(pos, direction) if direction == 'down' else 0
        if k in ('lever', 'button'):
            return 15 if (b.powered and self.lever_connected_direction(b) == direction) else 0
        if k == 'display':
            return self.block_signal(pos, direction) if direction == 'down' else 0
        return 0

    # ── the display block (redstone_plus; RedstoneComponents.cpp "DisplayBlock") ──
    def display_face_in(self, pos, d):
        """A face reads any adjacent dust carrying power, whatever its shape,
        and any other neighbour signalling toward it."""
        np_ = add(pos, d)
        b = self.blocks.get(np_)
        if b is None:
            return False
        if b.kind == 'wire':
            return b.power > 0
        return self.signal(np_, d) > 0

    def display_run(self, pos):
        """The pixel: the connected group (face neighbours) of display blocks
        containing pos, lowest block first (y, then z, then x) — the engine's
        DisplayGroup. Cached per group until invalidate_shapes()."""
        cache = self.__dict__.setdefault('_display_groups', {})
        g = cache.get(pos)
        if g is not None:
            return g
        group = [pos]; seen = {pos}
        i = 0
        while i < len(group):
            p = group[i]; i += 1
            pb = self.blocks[p]
            for d in N6:
                n = add(p, d)
                if n in seen or self.kind(n) != 'display':
                    continue
                # a latch's data face (north for op_nw, south for op_se) is a group boundary, from either side
                nb = self.blocks[n]
                if (d == 'north' and pb.op_nw == 'latch') or (d == 'south' and pb.op_se == 'latch') \
                        or (d == 'south' and nb.op_nw == 'latch') or (d == 'north' and nb.op_se == 'latch'):
                    continue
                seen.add(n); group.append(n)
        group.sort(key=lambda q: (q[1], q[2], q[0]))
        for q in group:
            cache[q] = group
        return group

    def display_eval(self, pos):
        """Evaluate the pixel containing pos; returns the positions whose
        state changed (the caller notifies their neighbours)."""
        if self.kind(pos) != 'display':
            return []
        run = self.display_run(pos)
        bits = self.blocks[run[0]].rgb
        if any(self.display_face_in(p, 'down') for p in run):
            bits = 0
        out = False
        for i, p in enumerate(run):
            b = self.blocks[p]
            two_up = (p[0], p[1] + 2, p[2])
            col_at = two_up if self.kind(two_up) == 'display' else p
            hits = (self.display_face_in(p, 'north') and self.display_face_in(col_at, 'west'),
                    self.display_face_in(p, 'south') and self.display_face_in(col_at, 'east'))
            for side, (op, hit) in enumerate(((b.op_nw, hits[0]), (b.op_se, hits[1]))):
                k = DISPLAY_OPS.index(op)
                if k <= 0:
                    continue
                if op == 'latch':                       # frame buffer: clocked by the column face, copies the group across the row face
                    if self.display_face_in(col_at, 'west' if side == 0 else 'east'):
                        src = self.blocks.get(add(p, 'north' if side == 0 else 'south'))
                        bits = src.rgb if (src is not None and src.kind == 'display') else 0
                    continue
                mask = 1 << ((k - 1) % 3)
                kind = (k - 1) // 3
                if kind == 0:
                    if hit: bits |= mask
                elif kind == 1:
                    if hit: bits &= ~mask
                elif kind == 2:
                    bits = (bits | mask) if hit else (bits & ~mask)
                elif hit and (bits & mask):
                    out = True
        changed = []
        setter = getattr(self, '_set', None)
        for i, p in enumerate(run):
            b = self.blocks[p]
            pw = out and self.kind(add(p, 'up')) != 'display'
            if b.rgb != bits or b.powered != pw:
                if setter is not None:
                    setter(p, b, 'rgb', bits); setter(p, b, 'powered', pw)
                else:
                    b.rgb = bits; b.powered = pw
                changed.append(p)
        return changed

    def display_bits(self, pos):
        return self.blocks[pos].rgb

    # ── SignalGetter ──────────────────────────────────────────────────
    def direct_signal_to(self, pos):
        best = 0
        for d in ('down', 'up', 'north', 'south', 'west', 'east'):
            best = max(best, self.block_direct_signal(add(pos, d), d))
            if best >= 15:
                return best
        return best

    def signal(self, pos, direction):
        s = self.block_signal(pos, direction)
        if self.is_conductor(pos):
            return max(s, self.direct_signal_to(pos))
        return s

    def has_signal(self, pos, direction):
        return self.signal(pos, direction) > 0

    def has_neighbor_signal(self, pos):
        for d in ('down', 'up', 'north', 'south', 'west', 'east'):
            if self.signal(add(pos, d), d) > 0:
                return True
        return False

    def best_neighbor_signal(self, pos):
        best = 0
        for d in ('down', 'up', 'north', 'south', 'west', 'east'):
            s = self.signal(add(pos, d), d)
            if s >= 15:
                return 15
            best = max(best, s)
        return best

    def control_input_signal(self, pos, direction, only_diodes):
        k = self.kind(pos)
        if only_diodes:
            return self.block_direct_signal(pos, direction) if self.is_diode(pos) else 0
        if k == 'wire':
            return self.blocks[pos].power
        if self.is_signal_source(pos):
            return self.block_direct_signal(pos, direction)
        return 0

    # ── redstone wire connection shape ────────────────────────────────
    def should_connect_to(self, pos, direction):
        """BlockState.shouldRedstoneWireConnectTo(level, pos, direction)."""
        b = self.blocks.get(pos)
        if b is None:
            return False
        if b.kind == 'wire':
            return True
        if b.kind == 'repeater':
            return direction is not None and (b.facing == direction or OPP[b.facing] == direction)
        return self.is_signal_source(pos) and direction is not None

    def connecting_side(self, pos, direction, can_connect_up):
        rel = add(pos, direction)
        if can_connect_up:
            placeable_above = self.wire_can_survive_on(rel)
            if placeable_above and self.should_connect_to(add(rel, 'up'), None):
                if self.face_sturdy_up(rel) and self.kind(rel) in ('solid', 'lamp', 'glow'):
                    # isFaceSturdy(direction.getOpposite()) — every block we place
                    # that can carry wire is a full cube, so all its faces are sturdy.
                    return 'up'
                return 'side'
        if (not self.should_connect_to(rel, direction) and
                (self.is_conductor(rel) or not self.should_connect_to(add(rel, 'down'), None))):
            return 'none'
        return 'side'

    def wire_sides(self, pos):
        """getConnectionState for a wire that was not a dot (all of ours)."""
        b = self.blocks[pos]
        if b.sides is not None:
            return b.sides
        can_up = not self.is_conductor(add(pos, 'up'))
        sides = {d: self.connecting_side(pos, d, can_up) for d in HORIZ}
        n = sides['north'] != 'none'
        s = sides['south'] != 'none'
        e = sides['east'] != 'none'
        w = sides['west'] != 'none'
        ns_empty = not n and not s
        ew_empty = not e and not w
        if not w and ns_empty:
            sides['west'] = 'side'
        if not e and ns_empty:
            sides['east'] = 'side'
        if not n and ew_empty:
            sides['north'] = 'side'
        if not s and ew_empty:
            sides['south'] = 'side'
        b.sides = sides
        return sides

    def invalidate_shapes(self):
        self.__dict__.pop('_display_groups', None)
        for b in self.blocks.values():
            if b.kind == 'wire':
                b.sides = None

    # ── redstone wire power (DefaultRedstoneWireEvaluator) ────────────
    def wire_power_of(self, pos):
        b = self.blocks.get(pos)
        return b.power if (b is not None and b.kind == 'wire') else 0

    def wire_target(self, pos):
        self.wires_muted = True
        block_signal = self.best_neighbor_signal(pos)
        self.wires_muted = False
        if block_signal == 15:
            return 15
        incoming = 0
        above_conductor = self.is_conductor(add(pos, 'up'))
        for d in HORIZ:
            np_ = add(pos, d)
            incoming = max(incoming, self.wire_power_of(np_))
            if self.is_conductor(np_) and not above_conductor:
                incoming = max(incoming, self.wire_power_of(add(np_, 'up')))
            elif not self.is_conductor(np_):
                incoming = max(incoming, self.wire_power_of(add(np_, 'down')))
        if getattr(self, 'no_decay', False):
            return max(block_signal, incoming)
        return max(block_signal, max(0, incoming - 1))

    def wire_readers(self, pos):
        """Wires whose target can depend on the wire at `pos`."""
        x, y, z = pos
        out = []
        for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            for dy in (-1, 0, 1):
                q = (x + dx, y + dy, z + dz)
                b = self.blocks.get(q)
                if b is not None and b.kind == 'wire':
                    out.append(q)
        return out

    def settle_wires(self):
        wires = [p for p, b in self.blocks.items() if b.kind == 'wire']
        for p in wires:
            self.blocks[p].power = 0
        # Least fixpoint from zero by a worklist: a wire is re-evaluated only
        # when something it reads may have changed.
        queue = deque(wires)
        queued = set(wires)
        while queue:
            p = queue.popleft(); queued.discard(p)
            t = self.wire_target(p)
            if t != self.blocks[p].power:
                self.blocks[p].power = t
                for q in self.wire_readers(p):
                    if q not in queued:
                        queued.add(q); queue.append(q)

    # ── component evaluation ──────────────────────────────────────────
    def diode_input(self, pos, b):
        rear = add(pos, b.facing)
        s = self.signal(rear, b.facing)
        if s >= 15:
            return s
        return max(s, self.wire_power_of(rear))

    def diode_alternate(self, pos, b):
        cw, ccw = CW[b.facing], CCW[b.facing]
        only = (b.kind == 'repeater')
        return max(self.control_input_signal(add(pos, cw), cw, only),
                   self.control_input_signal(add(pos, ccw), ccw, only))

    def comparator_output(self, pos, b):
        inp = self.diode_input(pos, b)
        if inp == 0:
            return 0
        alt = self.diode_alternate(pos, b)
        if alt > inp:
            return 0
        return inp - alt if b.mode == 'subtract' else inp

    def next_state(self, pos, b):
        """Returns the settled value of the block's dynamic field, or None."""
        if b.kind == 'torch':
            return not self.has_signal(add(pos, 'down'), 'down')
        if b.kind == 'wtorch':
            o = OPP[b.facing]
            return not self.has_signal(add(pos, o), o)
        if b.kind == 'repeater':
            if self.diode_alternate(pos, b) > 0:          # locked: holds its state
                return b.powered
            return self.diode_input(pos, b) > 0
        if b.kind == 'comparator':
            return self.comparator_output(pos, b)
        if b.kind == 'lamp':
            return self.has_neighbor_signal(pos)
        return None

    def resettle_wires_from(self, starts):
        """Recompute the wire nets containing `starts` (least fixpoint from
        zero, like settle_wires but local). Returns the wires that changed."""
        comp = []; seen = set()
        q = deque()
        for s in starts:
            if s not in seen:
                seen.add(s); comp.append(s); q.append(s)
        while q:
            p = q.popleft()
            for r in self.wire_readers(p):
                if r not in seen:
                    seen.add(r); comp.append(r); q.append(r)
        old = {p: self.blocks[p].power for p in comp}
        for p in comp:
            self.blocks[p].power = 0
        queue = deque(comp); queued = set(comp)
        while queue:
            p = queue.popleft(); queued.discard(p)
            t = self.wire_target(p)
            if t != self.blocks[p].power:
                self.blocks[p].power = t
                for r in self.wire_readers(p):
                    if r not in queued and r in seen:
                        queued.add(r); queue.append(r)
        return [p for p in comp if self.blocks[p].power != old[p]]

    def _reach(self, pos):
        """Every position a change at `pos` can be read from: the vanilla
        two-level neighbour reach (updateNeighborsAt on pos and its 6
        neighbours)."""
        out = set()
        for p in (pos,) + tuple(add(pos, d) for d in N6):
            out.add(p)
            for d in N6:
                out.add(add(p, d))
        return out

    def settle(self, max_rounds=200, pinned=(), max_flips=2000):
        """Event-driven fixpoint over wires + components. Returns the number
        of component changes applied. `pinned` positions are held at their
        current state (a way to force latches: nothing here can flip them).
        A component that keeps changing (a clock loop with no steady state)
        stops the iteration: positions that flipped more than `max_flips`
        times raise RuntimeError with the offenders."""
        self.invalidate_shapes()
        pinned = set(pinned)
        kinds = ('torch', 'wtorch', 'repeater', 'comparator', 'lamp', 'display')
        comps = [p for p, b in self.blocks.items() if b.kind in kinds and p not in pinned]
        self.settle_wires()
        queue = deque(comps); queued = set(comps)
        flips = {}
        applied = 0
        while queue:
            p = queue.popleft(); queued.discard(p)
            b = self.blocks[p]
            if b.kind == 'display':
                chg = self.display_eval(p)
                if not chg:
                    continue
                v = None
            else:
                v = self.next_state(p, b)
                if v is None:
                    continue
            if b.kind == 'display':
                pass
            elif b.kind in ('torch', 'wtorch', 'lamp'):
                if v == b.lit:
                    continue
                b.lit = v
            elif b.kind == 'repeater':
                if v == b.powered:
                    continue
                b.powered = v
            else:
                if v == b.output:
                    continue
                b.output = v
                b.powered = v > 0
            applied += 1
            n = flips.get(p, 0) + 1
            flips[p] = n
            if n > max_flips:
                hot = sorted(flips.items(), key=lambda kv: -kv[1])[:20]
                raise RuntimeError('circuit did not settle (oscillation?): ' + repr(hot))
            reach = self._reach(p)
            if b.kind == 'display':
                for q in chg:
                    reach |= self._reach(q)
            wires = [q for q in reach if q in self.blocks and self.blocks[q].kind == 'wire']
            changed = self.resettle_wires_from(wires) if wires else []
            for w in changed:
                reach |= self._reach(w)
            for q in reach:
                if q in pinned or q in queued:
                    continue
                bb = self.blocks.get(q)
                if bb is not None and bb.kind in kinds:
                    queued.add(q); queue.append(q)
        for p in comps:
            b = self.blocks[p]
            if b.kind == 'comparator':
                b.powered = b.output > 0
        return applied

    # ── helpers for tests ─────────────────────────────────────────────
    def set_lever(self, pos, on):
        self.blocks[pos].powered = bool(on)

    def lamp(self, pos):
        return self.blocks[pos].lit
