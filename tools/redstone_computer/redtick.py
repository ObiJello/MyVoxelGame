"""Tick-accurate redstone simulator (extends redsim.World).

Ports the vanilla scheduling rules on top of the steady-state signal rules:
  LevelTicks     one pending tick per (pos, block); ordered by (time, priority,
                 sequence); the set to run is fixed at the start of a tick
                 (willTickThisTick), so a tick scheduled while running lands
                 on a later tick.
  RedstoneTorch  neighborChanged: lit == hasNeighborSignal -> schedule 2.
                 tick: flip to match. (No burnout model.)
  DiodeBlock     checkTickOnNeighbor: !locked && on != shouldTurnOn ->
                 schedule(delay, EXTREMELY_HIGH if shouldPrioritize, else
                 VERY_HIGH if on, else HIGH). tick: turn off / turn on, and a
                 repeater turning on with the input already gone re-schedules
                 itself (minimum pulse = delay). Repeater delay = DELAY*2.
  Comparator     schedule 2 (HIGH if prioritised) when the output value or the
                 powered flag would change; tick = refreshOutputState.
  RedstoneLamp   turns on immediately, off after 4.
  Button         pressed for ticksToStayPressed (stone 20).
  Wire           settles synchronously inside the update; MC's evaluator then
                 notifies every changed wire, its neighbours, and THEIR
                 neighbours (updateNeighborsAt on each), which is how a torch
                 on a wire-powered block hears about it.
"""
import heapq
from collections import deque
from redsim import World, Block, add, DIRS, OPP, HORIZ

EXTREMELY_HIGH, VERY_HIGH, HIGH, NORMAL = -3, -2, -1, 0
N6 = ('down', 'up', 'north', 'south', 'west', 'east')


class TickWorld(World):
    def __init__(self):
        super().__init__()
        self.time = 0
        self._heap = []
        self._pending = set()      # (pos, kind)
        self._running = set()      # (pos, kind) fixed for the current tick
        self._seq = 0
        self.trace = None          # optional list collecting (time, pos, kind, field, value)
        self._instant_flips = {}   # blue torches: flips per position this tick

    # ── scheduling ──────────────────────────────────────────────────────
    def schedule(self, pos, kind, delay, priority=NORMAL):
        key = (pos, kind)
        if key in self._pending:
            return
        self._pending.add(key)
        self._seq += 1
        heapq.heappush(self._heap, (self.time + delay, priority, self._seq, pos, kind))

    def will_tick_this_tick(self, pos, kind):
        return (pos, kind) in self._running

    def tick(self, n=1):
        for _ in range(n):
            self.time += 1
            due = []
            while self._heap and self._heap[0][0] <= self.time:
                due.append(heapq.heappop(self._heap))
            self._running = {(d[3], d[4]) for d in due}
            for _, _, _, pos, kind in due:
                # LevelTicks.runCollectedTicks drops each entry from the
                # this-tick set as it runs, so a component that already ticked
                # can be re-scheduled by a later event in the same tick.
                self._running.discard((pos, kind))
                self._pending.discard((pos, kind))
                b = self.blocks.get(pos)
                if b is None or b.kind != kind:
                    continue
                self._block_tick(pos, b)
            self._running = set()
            self._flush_deferred()

    def boot(self):
        """Start like a freshly loaded save whose blocks all get a neighbour
        update: wires settled against the current component states, then
        every component runs its neighborChanged check, which schedules a
        tick only where the state is inconsistent (a repeater's tick ALWAYS
        turns it on, so blindly scheduling one would glitch every idle
        repeater). Unlike settle(), this works for circuits with feedback."""
        self.invalidate_shapes()
        self.settle_wires()
        for p, b in list(self.blocks.items()):
            if b.kind in ('torch', 'wtorch', 'repeater', 'comparator', 'lamp', 'display'):
                self._neighbor_changed(p)
        self._flush_deferred()

    def run_until_idle(self, limit=10000):
        """Tick until no scheduled ticks remain (never true for a clock)."""
        n = 0
        while self._heap and n < limit:
            self.tick(); n += 1
        return n

    # ── state changes ───────────────────────────────────────────────────
    def _set(self, pos, b, field, value):
        setattr(b, field, value)
        if self.trace is not None:
            self.trace.append((self.time, pos, b.kind, field, value))

    def _notify_around(self, pos):
        """updateNeighborsAt(pos) plus the same for each neighbour — the
        two-level reach every vanilla source uses (torch notifyNeighbors,
        diode updateNeighborsInFront, wire evaluator toUpdate)."""
        seen = set()
        for p in (pos,) + tuple(add(pos, d) for d in N6):
            for d in N6:
                q = add(p, d)
                if q in seen or q == pos:
                    continue
                seen.add(q)
                self._neighbor_changed(q)

    def _neighbor_changed(self, pos):
        b = self.blocks.get(pos)
        if b is None:
            return
        k = b.kind
        if k == 'wire':
            self._resettle_wire(pos)
        elif k in ('torch', 'wtorch'):
            if getattr(b, 'instant', False) and self.no_decay:
                if b.lit != self._torch_has_signal(pos, b):
                    return
                # blue torch: flip inside this update, up to 8 times a tick per
                # torch (the engine's kMaxInstantFlipsPerTick); a loop degrades
                # to the scheduled flip instead of recursing forever
                flips = self.__dict__.setdefault('_instant_flips', {})
                if flips.get('t') != self.time:
                    flips.clear(); flips['t'] = self.time
                n = flips.get(pos, 0)
                if n < 8:
                    flips[pos] = n + 1
                    self._set(pos, b, 'lit', not b.lit); self._notify_around(pos)
                    return
            if self.no_decay:
                self._defer(pos); return
            self._check(pos, b)
        elif k in ('repeater', 'comparator', 'lamp', 'display'):
            if self.no_decay:
                self._defer(pos); return
            self._check(pos, b)

    def _defer(self, pos):
        """redstone_plus: delayed components re-check only once the cascade has
        settled (end of the tick / after an input event) — order-independent,
        glitch-free; mirrors RedstoneComponents.cpp's deferred re-checks."""
        d = self.__dict__.setdefault('_deferred', [])
        ds = self.__dict__.setdefault('_deferred_set', set())
        if pos not in ds:
            ds.add(pos); d.append(pos)

    def _flush_deferred(self):
        for _ in range(8):
            d = self.__dict__.get('_deferred')
            if not d:
                return
            lst = list(d); d.clear(); self._deferred_set.clear()
            for pos in lst:
                b = self.blocks.get(pos)
                if b is not None and b.kind in ('torch', 'wtorch', 'repeater', 'comparator', 'lamp', 'display'):
                    self._check(pos, b)

    def _check(self, pos, b):
        """The vanilla neighborChanged decision of a delayed component."""
        k = b.kind
        if k in ('torch', 'wtorch'):
            if b.lit != self._torch_has_signal(pos, b):
                return
            if not self.will_tick_this_tick(pos, k):
                self.schedule(pos, k, 2)
        elif k == 'repeater':
            if self._repeater_locked(pos, b):
                return
            on = b.powered
            should = self.diode_input(pos, b) > 0
            if on != should and not self.will_tick_this_tick(pos, k):
                pr = HIGH
                if self._should_prioritize(pos, b):
                    pr = EXTREMELY_HIGH
                elif on:
                    pr = VERY_HIGH
                self.schedule(pos, k, b.delay * 2, pr)
        elif k == 'comparator':
            if self.will_tick_this_tick(pos, k):
                return
            new = self.comparator_output(pos, b)
            if new != b.output or b.powered != self._comparator_should_turn_on(pos, b):
                self.schedule(pos, k, 2, HIGH if self._should_prioritize(pos, b) else NORMAL)
        elif k == 'lamp':
            has = self.has_neighbor_signal(pos)
            if b.lit != has:
                if b.lit:
                    self.schedule(pos, k, 4)
                else:
                    self._set(pos, b, 'lit', True)
        elif k == 'display':
            for q in self.display_eval(pos):        # setBlockAndUpdate on every changed block of the run
                self._notify_around(q)

    def _torch_has_signal(self, pos, b):
        if b.kind == 'torch':
            return self.has_signal(add(pos, 'down'), 'down')
        o = OPP[b.facing]
        return self.has_signal(add(pos, o), o)

    def _repeater_locked(self, pos, b):
        return self.diode_alternate(pos, b) > 0      # sideInputDiodesOnly for repeaters

    def _comparator_should_turn_on(self, pos, b):
        inp = self.diode_input(pos, b)
        if inp == 0:
            return False
        side = self.diode_alternate(pos, b)
        return inp > side or (inp == side and b.mode == 'compare')

    def _should_prioritize(self, pos, b):
        front = add(pos, OPP[b.facing])
        fb = self.blocks.get(front)
        return fb is not None and fb.kind in ('repeater', 'comparator') and fb.facing != OPP[b.facing]

    # ── wires ───────────────────────────────────────────────────────────
    def _resettle_wire(self, start):
        comp = [start]; seen = {start}
        q = deque([start])
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
        changed = [p for p in comp if self.blocks[p].power != old[p]]
        if not changed:
            return
        if self.trace is not None:
            for p in changed:
                self.trace.append((self.time, p, 'wire', 'power', self.blocks[p].power))
        notify = set()
        for w in changed:
            for p in (w,) + tuple(add(w, d) for d in N6):
                for d in N6:
                    qq = add(p, d)
                    if qq not in seen:
                        notify.add(qq)
        for p in notify:
            self._neighbor_changed(p)

    # ── block ticks ─────────────────────────────────────────────────────
    def _block_tick(self, pos, b):
        k = b.kind
        if k in ('torch', 'wtorch'):
            sig = self._torch_has_signal(pos, b)
            if b.lit and sig:
                self._set(pos, b, 'lit', False); self._notify_around(pos)
            elif not b.lit and not sig:
                self._set(pos, b, 'lit', True); self._notify_around(pos)
        elif k == 'repeater':
            if self._repeater_locked(pos, b):
                return
            on = b.powered
            should = self.diode_input(pos, b) > 0
            if on and not should:
                self._set(pos, b, 'powered', False); self._notify_around(pos)
            elif not on:
                self._set(pos, b, 'powered', True); self._notify_around(pos)
                if not should:
                    self.schedule(pos, k, b.delay * 2, VERY_HIGH)
        elif k == 'comparator':
            new = self.comparator_output(pos, b)
            old = b.output
            b.output = new
            if old != new or b.mode == 'compare':
                should = self._comparator_should_turn_on(pos, b)
                if b.powered != should:
                    self._set(pos, b, 'powered', should)
                if self.trace is not None and old != new:
                    self.trace.append((self.time, pos, k, 'output', new))
                self._notify_around(pos)
        elif k == 'lamp':
            if b.lit and not self.has_neighbor_signal(pos):
                self._set(pos, b, 'lit', False)
        elif k == 'button':
            if b.powered:
                self._set(pos, b, 'powered', False); self._notify_around(pos)

    # ── inputs ──────────────────────────────────────────────────────────
    def toggle_lever(self, pos, on=None):
        b = self.blocks[pos]
        self._set(pos, b, 'powered', (not b.powered) if on is None else bool(on))
        self._notify_around(pos)
        self._flush_deferred()

    def press_button(self, pos):
        b = self.blocks[pos]
        if b.powered:
            return
        self._set(pos, b, 'powered', True)
        self._notify_around(pos)
        self.schedule(pos, 'button', 20)
        self._flush_deferred()

    # ── save-file view ──────────────────────────────────────────────────
    def repeater_locked_now(self, pos):
        return self._repeater_locked(pos, self.blocks[pos])
