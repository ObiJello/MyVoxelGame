"""Initialise and play Snake v2 (snake2.Machine2) in the tick simulator.

    python3 play2.py trace [ticks] [--pickle raw.pkl]   lane-edge trace
    python3 play2.py play  [steps] [--pickle raw.pkl]   compare with snakemodel
"""
import sys, pickle, random, time
from build import Y
from pixel import L4, L5
from snake2 import Machine2, N, STAGES
from snakemodel import Snake


def set_repeater(w, pos, on):
    w.get(pos).powered = bool(on)


def board(m):
    w = m.w
    if 'pixel' in m.ports:                  # display-block panel: green = body
        return {(i, j) for i in range(N) for j in range(N) if w.display_bits(m.ports['pixel'][(i, j)]) & 2}
    return {(i, j) for i in range(N) for j in range(N) if w.lamp(m.ports['body'][(i, j)])}


def foods(m):
    w = m.w
    if 'pixel' in m.ports:                  # display-block panel: red = food
        return {(i, j) for i in range(N) for j in range(N) if w.display_bits(m.ports['pixel'][(i, j)]) & 1}
    return {(i, j) for i in range(N) for j in range(N) if w.lamp(m.ports['food'][(i, j)])}


def reg(m, name, bits):
    w = m.w
    return sum(int(w.get(m.regs[f'{name}{i}']).powered) << i for i in range(bits))


def head_pos(m): return (reg(m, 'HX', 3), reg(m, 'HY', 3))
def tail_pos(m): return (reg(m, 'TX', 3), reg(m, 'TY', 3))
def food_pos(m):
    inv = 7 if getattr(m, 'food_inverted', False) else 0
    return (reg(m, 'FX', 3) ^ inv, reg(m, 'FY', 3) ^ inv)
def length(m): return reg(m, 'L', 4)
def dead_lamp(m): return m.w.lamp(getattr(m, 'dead_lamp', (m.dead_q.x, Y - 1, m.dead_q.z0)))
def has_wall(m): return hasattr(m, 'screen') or 'wall' in m.ports
def wall_board(m):
    if 'wall' in m.ports:                   # display-block frame buffer: green = body
        return {k for k, p in m.ports['wall'].items() if m.w.display_bits(p) & 2}
    return {k for k, p in m.screen['body'].items() if m.w.lamp(p)} if hasattr(m, 'screen') else None
def wall_foods(m):
    if 'wall' in m.ports:                   # red = food
        return {k for k, p in m.ports['wall'].items() if m.w.display_bits(p) & 1}
    return {k for k, p in m.screen['food'].items() if m.w.lamp(p)} if hasattr(m, 'screen') else None
def dir_val(m): return (int(m.w.get(m.regs['DIR0']).powered), int(m.w.get(m.regs['DIR1']).powered))


def force_ripple_counter(m, name, x0, z0, bits, value):
    w = m.w
    for i in range(bits):
        sx = x0 + 18 * i
        q = (value >> i) & 1
        w.get((sx + 4, Y, z0)).powered = bool(q)
        w.get((sx + 1, Y, z0)).lit = not q
        if i + 1 < bits:
            px, pz = sx + 18 - 3, z0 + 8
            a = not q
            w.get((px, Y + 1, pz - 2)).lit = not a
            w.get((px + 1, Y, pz)).powered = a
            w.get((px + 4, Y, pz - 1)).lit = False


def pixel_torches(m, i, j):
    """(TA, TB) wall torches of pixel (i, j): TA lit <=> off, TB lit <=> on."""
    from pixel import W as PW
    x0, z0 = 16 * i, 16 * j
    return (x0 + 5, L4, z0 + 2), (x0 + 10, L4, z0 + 12)


def init_state(m, head=None, food=None, direction=None):
    w = m.w
    init = getattr(m, 'init', {'head': (3, 3), 'food': (5, 3), 'dir': 1})
    head = init['head'] if head is None else head
    food = init['food'] if food is None else food
    direction = init['dir'] if direction is None else direction
    dbits = (direction & 1, (direction >> 1) & 1)
    hx, hy = head

    def rs(name_q, on):
        lane = m.f.lanes[name_q]
        xa = lane.x - 9; z = lane.z0 - 1
        w.get((xa + 1, Y, z)).lit = not on
        w.get((xa + 7, Y, z)).lit = bool(on)

    def force_regs():
        for i in range(3):
            set_repeater(w, m.regs[f'HX{i}'], (hx >> i) & 1)
            set_repeater(w, m.regs[f'HY{i}'], (hy >> i) & 1)
            set_repeater(w, m.regs[f'TX{i}'], (hx >> i) & 1)
            set_repeater(w, m.regs[f'TY{i}'], (hy >> i) & 1)
            inv = 1 if getattr(m, 'food_inverted', False) else 0
            set_repeater(w, m.regs[f'FX{i}'], ((food[0] >> i) & 1) ^ inv)
            set_repeater(w, m.regs[f'FY{i}'], ((food[1] >> i) & 1) ^ inv)
        lx, lz = m.regs['L0'][0] - 4, m.regs['L0'][2]
        force_ripple_counter(m, 'L', lx, lz, 4, 1)
        if 'REQ0' in m.regs:
            set_repeater(w, m.regs['REQ0'], dbits[0]); set_repeater(w, m.regs['REQ1'], dbits[1])
        else:
            rs('REQ0', bool(dbits[0])); rs('REQ1', bool(dbits[1]))                     # v3: RS latches
        set_repeater(w, m.regs['DIR0'], dbits[0]); set_repeater(w, m.regs['DIR1'], dbits[1])
        # the direction FIFO: every stage holds "east" (v3 stores NOT(direction))
        if hasattr(m, 'mux'):
            from snake3 import ZB, STAGES
            l_lines, fifo_q, mux_ztop, td_x = m.mux
            for bit, val in ((0, dbits[0]), (1, dbits[1])):        # the stages store NOT(direction bit)
                for k, xq in enumerate(fifo_q[bit]):
                    set_repeater(w, (xq - 2, Y, ZB - 110 + 8 * k), not val)
    def fifo_pins():
        if not hasattr(m, 'mux'):
            return []
        from snake3 import ZB
        l_lines, fifo_q, mux_ztop, td_x = m.mux
        return [(xq - 2, Y, ZB - 110 + 8 * k) for bit in range(2) for k, xq in enumerate(fifo_q[bit])]

    def rs(name_q, on):
        lane = m.f.lanes[name_q]
        xa = lane.x - 9; z = lane.z0 - 1
        w.get((xa + 1, Y, z)).lit = not on
        w.get((xa + 7, Y, z)).lit = bool(on)

    def force_board():
        rs('DEAD', False)
        for i in range(N):
            for j in range(N):
                on = (i, j) == head
                if 'pixel' in m.ports:                      # display-block panel: the run's stored bits
                    from panel import RUN
                    px, py, pz = m.ports['pixel'][(i, j)]
                    for y in range(RUN):
                        blk = w.get((px, py + y, pz)); blk.rgb = 2 if on else 0; blk.powered = False
                    continue
                ta, tb = pixel_torches(m, i, j)
                w.get(ta).lit = not on
                w.get(tb).lit = on

    def pinned():
        pins = list(m.regs.values()) + fifo_pins()
        for nm in ('DEAD', 'REQ0', 'REQ1'):
            if nm in m.f.lanes and nm not in m.regs:
                lane = m.f.lanes[nm]; xa = lane.x - 9; z = lane.z0 - 1
                pins += [(xa + 1, Y, z), (xa + 7, Y, z)]
        if 'pixel' in m.ports:
            from panel import RUN
            for (px, py, pz) in m.ports['pixel'].values():
                pins += [(px, py + y, pz) for y in range(RUN)]
        else:
            for i in range(N):
                for j in range(N):
                    pins += list(pixel_torches(m, i, j))
        return pins

    for lv in m.pause:
        w.get(lv).powered = True
    force_regs(); force_board()
    w.invalidate_shapes()
    t0 = time.time()
    n = w.settle(max_rounds=200, pinned=pinned())
    print('  settled: %d changes, %.0fs' % (n, time.time() - t0))
    report(m, 'after settle (paused)')
    w.trace = []
    w.boot(); w.run_until_idle(limit=500)
    print('  boot events', len(w.trace)); w.trace = None
    report(m, 'paused')


def report(m, tag):
    print(f'  [{tag}] head {head_pos(m)} tail {tail_pos(m)} food {food_pos(m)} L {length(m)} dir {dir_val(m)} '
          f'board {sorted(board(m))} foods {sorted(foods(m))} dead {dead_lamp(m)}')


def unpause(m):
    for lv in m.pause:
        m.w.toggle_lever(lv, False)


def load(pickle_path):
    sys.setrecursionlimit(100000)
    if pickle_path:
        return pickle.load(open(pickle_path, 'rb'))
    return Machine2()


class Recorder:
    """Edge recorder over every fabric lane (probed at its first cell) plus a
    few board-side cells. start() marks t0; tick(n) advances and records;
    dump(path) writes 'time<TAB>name<TAB>value' lines."""
    def __init__(self, m):
        self.m, w, f = m, m.w, m.f
        self.probe = {}
        for n, l in f.lanes.items():
            p = (l.x, Y, l.z0)
            bb = w.blocks.get(p)
            if bb is not None and bb.kind == 'wire':
                self.probe[n] = p
        # a few board-side probes: the first HX column cell, row 3's HYS/HYC/TYE cells at the board edge
        self.probe['col:HX3'] = m.ports['HX'][3]
        self.probe['col:HX4'] = m.ports['HX'][4]
        self.probe['row:HYS3'] = m.ports['HYS'][3]
        self.probe['row:HYC3'] = m.ports['HYC'][3]
        self.probe['row:TYE3'] = m.ports['TYE'][3]
        self.edges = []
        self.active = False

    def start(self):
        w = self.m.w
        self.t0 = w.time
        self.state = {n: w.get(p).power > 0 for n, p in self.probe.items()}
        self.active = True

    def tick(self, n=1):
        w = self.m.w
        for _ in range(n):
            w.tick()
            if not self.active:
                continue
            for nm, p in self.probe.items():
                cur = w.get(p).power > 0
                if cur != self.state[nm]:
                    self.state[nm] = cur; self.edges.append((w.time - self.t0, nm, cur))

    def dump(self, out):
        with open(out, 'w') as fh:
            for t, n, v in self.edges:
                fh.write(f'{t}\t{n}\t{int(v)}\n')
        print('edges', len(self.edges), '->', out)


def trace(m, ticks, out, press=None):
    """press: (tick, 'N'|'E'|'S'|'W') button press during the trace."""
    w = m.w
    rec = Recorder(m); rec.start()
    for i in range(ticks):
        rec.tick()
        for pt, pd in (press or []):
            if i == pt:
                w.press_button(m.buttons[pd]); print('  pressed', pd, 'at', w.time - rec.t0)
        if i % 100 == 99:
            report(m, f't {w.time - rec.t0}')
    rec.dump(out)


def wait_step(m, pa, settle, limit=3000):
    w = m.w
    cell = (pa.x, Y, pa.z0)
    prev = w.get(cell).power > 0
    for _ in range(limit):
        w.tick()
        cur = w.get(cell).power > 0
        if cur and not prev:
            break
        prev = cur
    else:
        return False
    w.tick(settle)
    return True


def greedy(model, pending, rng):
    """A direction for the NEXT move (the machine turns one step later than
    the press): head towards the food, never into a wall or the body,
    with a little randomness."""
    if model.dead:
        return None
    hx, hy = model.body[0]
    cur = pending if pending is not None else model.dir
    dx, dy = Snake.DIRS[cur]
    nx, ny = hx + dx, hy + dy            # where the head will be after the move already latched
    body = set(model.body)
    scores = []
    for d, (ddx, ddy) in Snake.DIRS.items():
        if (d - cur) % 4 == 2:
            continue
        tx, ty = nx + ddx, ny + ddy
        if not (0 <= tx < 8 and 0 <= ty < 8) or (tx, ty) in body:
            continue
        dist = abs(tx - model.food[0]) + abs(ty - model.food[1]) if model.food else 0
        scores.append((dist + rng.random() * 1.5, d))
    if not scores:
        return None
    scores.sort()
    return scores[0][1]


def play(m, steps, seed=1, settle=300, policy='random', rec=None):
    """rec: (step, path) records lane edges during that step's cycle (from its A edge) to path."""
    init = getattr(m, 'init', {'head': (3, 3), 'food': (5, 3), 'dir': 1})
    model = Snake(head=init['head'], food=init['food'], direction=init['dir'])
    pa = m.P[0]
    rng = random.Random(seed)
    ok = True
    pending = None                    # a button pressed early in cycle k (before E) moves the head in cycle k+1
    recorder = Recorder(m) if rec else None
    late = None                       # (step, tail, length, food) of the previous step: the tail step, L++ and
    for s in range(steps):            # the food write land after the read point, so they are checked ~A+150 of the next cycle
        if not wait_step(m, pa, 0):
            print('clock stopped: no A edge'); ok = False; break
        if rec and s + 1 == rec[0]:
            recorder.start()
        tick = recorder.tick if (rec and s + 1 == rec[0]) else m.w.tick
        tm = getattr(m, 'timing', {'PRESS': 100, 'LATE': 250})
        PRESS, LATE = tm['PRESS'], tm['LATE']   # the food write lands ~A+60 of the next cycle, so the food can be read
        tick(PRESS)                             # and the next press chosen at A+100; a deck press reaches the DIR
        if model.food is None:                  # register ~200 ticks later, before it is latched at B+56 = A+368.
            model.food = food_pos(m)            # The tail step (E+66) lands its slowest bit ~A+140: tail/len at A+250.
        req = None
        if policy == 'greedy':
            d = greedy(model, pending, rng)
            if d is not None and d != (pending if pending is not None else model.dir):
                req = d
        elif rng.random() < 0.5:
            opts = [d for d in range(4) if (d - model.dir) % 4 != 2]
            req = rng.choice(opts)
        if req is not None:
            m.w.press_button(m.buttons['NESW'[req]])
        tick(LATE - PRESS)
        if late is not None:
            ls, ltail, llen, lfood, lboard, lfoods = late
            lchecks = {'tail': (tail_pos(m), ltail), 'len': (length(m), llen)}
            if lfood is not None:
                lchecks['food'] = (food_pos(m), lfood)
            if has_wall(m):                         # the wall lags the board (lamp wall ~150 ticks; the frame buffer is
                lchecks['wall'] = (sorted(wall_board(m)), sorted(lboard))   # presented at E = A+200): compare with the previous read
                if tm.get('WALLFOOD') == 'late':    # plus timing: the food decode settles after the main read
                    lchecks['wallfood'] = (sorted(wall_foods(m)), sorted(foods(m)))
            lbad = [k for k, (a_, b_) in lchecks.items() if a_ != b_]
            if lbad:
                ok = False
                print(f'step {ls:2d} late   tail {tail_pos(m)} len {length(m)} food {food_pos(m)} : MISMATCH '
                      + ' '.join(f'{k}: got {lchecks[k][0]} want {lchecks[k][1]}' for k in lbad), flush=True)
        tick(settle - LATE)
        if rec and s + 1 == rec[0]:
            recorder.dump(rec[1])
        model.step(pending, next_food=None)
        pending = req
        got = board(m); dead = dead_lamp(m)
        checks = {'head': (head_pos(m), model.body[0]),
                  'board': (sorted(got), sorted(model.board())),
                  'dead': (dead, model.dead)}
        if has_wall(m) and tm.get('WALLFOOD') != 'late':   # the food changed ~A+40 of this cycle: the wall has caught up
            checks['wallfood'] = (sorted(wall_foods(m)), sorted(foods(m)))
        late = None if model.dead else (s + 1, model.body[-1], model.length, model.food, got, foods(m))
        if model.dead:
            checks.pop('head')
            if model.death == 'collision':
                checks.pop('dead')                # the collision check lands DEAD ~A+660: verified a cycle later below
            if model.death == 'collision':
                # the set pulse lands before the check's result can stop it: the crashed-into cell lights
                checks['board'] = (sorted(got), sorted(model.board() | {model.crash}))
        bad = [k for k, (a, b) in checks.items() if a != b]
        if bad: ok = False
        status = 'ok' if not bad else 'MISMATCH ' + ' '.join(f'{k}: got {checks[k][0]} want {checks[k][1]}' for k in bad)
        print(f'step {s + 1:2d} req={req} head {head_pos(m)} tail {tail_pos(m)} len {length(m)} food {food_pos(m)} '
              f'board {sorted(got)} dead {dead} : {status}', flush=True)
        if model.dead:
            # a wall death latches DEAD at A+88 and the clock stops before the next A; a collision
            # latches at ~A+660, so the clock runs one more cycle (the board is frozen by then)
            extra = 0
            for _ in range(3):
                if not wait_step(m, pa, 200):
                    break
                extra += 1
            else:
                print('clock still running after death'); ok = False
            if extra > (1 if model.death == 'collision' else 0):
                print(f'clock ran {extra} more cycles after death'); ok = False
            if not dead_lamp(m):
                print('DEAD lamp not lit'); ok = False
            if sorted(board(m)) != sorted(model.board() | ({model.crash} if model.crash else set())):
                print('board changed after death:', sorted(board(m))); ok = False
            break
    print('PASS' if ok else 'FAIL')
    return ok


def restart_test(m, seed=1, steps1=8, steps2=8):
    """Play until a death (or steps1 steps), flip the lever on (reset), wait,
    flip it off and play a fresh game: it must start from the initial state."""
    w = m.w
    print('--- game 1'); play(m, steps1, seed=seed, settle=m.timing['SETTLE'], policy='random')
    w.toggle_lever(m.pause[1], True)
    w.tick(600)                                             # the length counter counts back to 1 at RCLK pace
    report(m, 'after reset')
    init = getattr(m, 'init', {'head': (3, 3)})
    ok = head_pos(m) == init['head'] and tail_pos(m) == init['head'] and length(m) == 1 and not dead_lamp(m) and board(m) == set()
    print('reset state ok' if ok else 'RESET STATE WRONG')
    w.toggle_lever(m.pause[1], False)
    print('--- game 2'); ok2 = play(m, steps2, seed=seed + 1, settle=m.timing['SETTLE'], policy='greedy')
    print('RESTART PASS' if ok and ok2 else 'RESTART FAIL')
    return ok and ok2


if __name__ == '__main__':
    args = sys.argv[1:]
    pk = None
    if '--pickle' in args:
        i = args.index('--pickle'); pk = args[i + 1]; del args[i:i + 2]
    settle = None
    if '--settle' in args:
        i = args.index('--settle'); settle = int(args[i + 1]); del args[i:i + 2]
    seed = 1
    if '--seed' in args:
        i = args.index('--seed'); seed = int(args[i + 1]); del args[i:i + 2]
    rec = None
    if '--rec' in args:
        i = args.index('--rec'); rec = (int(args[i + 1]), args[i + 2]); del args[i:i + 3]
    policy = 'random'
    if '--greedy' in args:
        args.remove('--greedy'); policy = 'greedy'
    mode = args[0] if args else 'trace'
    n = int(args[1]) if len(args) > 1 else (2000 if mode == 'trace' else 8)
    m = load(pk)
    init_state(m)
    if '--slow' not in args:
        import fastsim; fastsim.attach(m.w)          # the C engine (fastsim.py); --slow keeps the Python ticker
    else:
        args.remove('--slow')
    unpause(m)
    if mode == 'trace':
        out = args[2] if len(args) > 2 else 'edges2.txt'
        trace(m, n, out)
    elif mode == 'restart':
        restart_test(m, seed=seed)
    else:
        if settle is None:
            settle = getattr(m, 'timing', {}).get('SETTLE', 300)
        play(m, n, seed=seed, settle=settle, policy=policy, rec=rec)
