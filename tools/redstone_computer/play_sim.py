"""Plays Snake on the simulated machine and checks every step against the
reference model.

    python3 play_sim.py [steps]
"""
import sys, random
from build import Y
from snake_machine import Machine, GX0, GZ0, STAGES
from snakemodel import Snake
from pcb import MID


def set_repeater(w, pos, on):
    w.get(pos).powered = bool(on)


def init_state(m, head=(3, 3), food=(5, 3)):
    """Put the machine into the starting position without ever running it:
    the registers and latches are forced, then the steady-state solver
    (redsim.World.settle, which respects repeater locks) makes every torch,
    repeater and wire consistent with them. The clock is held off through
    DEAD during that; releasing DEAD afterwards starts the game cleanly."""
    w = m.w
    hx, hy = head

    def force_regs():
        for i in range(3):
            set_repeater(w, m.regs[f'HX{i}'], (hx >> i) & 1)
            set_repeater(w, m.regs[f'HY{i}'], (hy >> i) & 1)
            set_repeater(w, m.regs[f'TX{i}'], (hx >> i) & 1)
            set_repeater(w, m.regs[f'TY{i}'], (hy >> i) & 1)
            set_repeater(w, m.regs[f'FX{i}'], (food[0] >> i) & 1)
            set_repeater(w, m.regs[f'FY{i}'], (food[1] >> i) & 1)
        for i in range(4):
            set_repeater(w, m.regs[f'L{i}'], (1 >> i) & 1)        # length 1
        set_repeater(w, m.regs['REQ0'], 1); set_repeater(w, m.regs['REQ1'], 0)   # east
        set_repeater(w, m.regs['DIR0'], 1); set_repeater(w, m.regs['DIR1'], 0)

    def rs(name_q, on):
        lane = m.f.lanes[name_q]
        xa = lane.x - 9; z = lane.z0 - 1
        w.get((xa + 1, Y, z)).lit = not on
        w.get((xa + 7, Y, z)).lit = bool(on)

    def force_latches(dead):
        rs('GROW', False); rs('DEAD', dead)
        for (i, j), c in m.cells.items():
            on = (i, j) == head
            cx, cz = GX0 + 32 * i, GZ0 + 32 * j
            w.get((cx + 19, MID + 1, cz + 28)).lit = not on     # TA
            w.get((cx + 23, MID + 1, cz + 28)).lit = bool(on)   # TB
            w.get((cx + 17, MID + 1, cz + 28)).lit = not on     # Q_L torch

    def pinned(dead):
        pins = list(m.regs.values())
        for name_q in ('GROW', 'DEAD'):
            lane = m.f.lanes[name_q]; xa = lane.x - 9; z = lane.z0 - 1
            pins += [(xa + 1, Y, z), (xa + 7, Y, z)]
        for (i, j), c in m.cells.items():
            cx, cz = GX0 + 32 * i, GZ0 + 32 * j
            pins += [(cx + 19, MID + 1, cz + 28), (cx + 23, MID + 1, cz + 28), (cx + 17, MID + 1, cz + 28)]
        return pins

    def steady():
        w.invalidate_shapes()
        rounds = w.settle(max_rounds=200, pinned=pinned(True))
        print('  settled in', rounds, 'rounds')

    def report(tag):
        print(f'  [init {tag}] head {head_pos(m)} tail {tail_pos(m)} food {food_pos(m)} L {length(m)} '
              f'board {sorted(board(m))} dead {w.lamp((m.dead_q.x, Y - 1, m.dead_q.z0))}')

    for lv in m.pause:                       # clocks stopped: the whole machine has a steady state
        w.get(lv).powered = True
    force_regs(); force_latches(dead=False); steady()
    report('after settle (paused)')
    w.boot(); w.run_until_idle(limit=200)
    report('paused')
    if getattr(m, 'pickle_path', None):
        import pickle, sys
        sys.setrecursionlimit(100000)
        pickle.dump(m, open(m.pickle_path, 'wb'), protocol=5)


def unpause(m):
    w = m.w
    for lv in m.pause:
        w.toggle_lever(lv, False)


def board(m):
    return {(i, j) for (i, j), c in m.cells.items() if m.w.lamp(c['body_lamp'])}


def food_pos(m):
    w = m.w
    fx = sum((w.get(m.regs[f'FX{i}']).powered) << i for i in range(3))
    fy = sum((w.get(m.regs[f'FY{i}']).powered) << i for i in range(3))
    return (fx, fy)


def tail_pos(m):
    w = m.w
    tx = sum((w.get(m.regs[f'TX{i}']).powered) << i for i in range(3))
    ty = sum((w.get(m.regs[f'TY{i}']).powered) << i for i in range(3))
    return (tx, ty)


def length(m):
    return sum((m.w.get(m.regs[f'L{i}']).powered) << i for i in range(4))


def head_pos(m):
    w = m.w
    hx = sum((w.get(m.regs[f'HX{i}']).powered) << i for i in range(3))
    hy = sum((w.get(m.regs[f'HY{i}']).powered) << i for i in range(3))
    return (hx, hy)


def wait_step(m, p1, settle=250, limit=1200):
    """Tick to the next rising edge of P1 (False if none comes within
    `limit` ticks: the clock is gated off, the snake is dead), then `settle`
    ticks into the cycle. Read there, the machine shows: head and tail
    registers after THIS cycle's P2/P4 (about +16 and +128 ticks), and the
    board, length, food and DEAD after the PREVIOUS cycle (its P6 set lands
    at about +130..+180 of this cycle, its P7 writes at about +350 of its
    own; this cycle's P4 erase lands at about +390, after the read)."""
    w = m.w
    cell = (p1.x, Y, p1.z0)                 # the lane's first cell is always plain wire
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


def dead_lamp(m):
    return m.w.lamp((m.dead_q.x, Y - 1, m.dead_q.z0))


def main(steps=12, seed=1, pickle_path=None):
    if pickle_path:
        import pickle
        sys.setrecursionlimit(100000)
        m = pickle.load(open(pickle_path, 'rb'))
    else:
        m = Machine()
    init_state(m)
    unpause(m)
    model = Snake(head=(3, 3), food=(5, 3), direction=1)
    p1 = m.P[0]
    rng = random.Random(seed)
    print('start board', sorted(board(m)), 'head', head_pos(m), 'food', food_pos(m))
    ok = True
    for s in range(steps):
        # maybe press a button (avoid immediate reversal); the request is
        # latched into REQ at once and into DIR at the next P1
        req = None
        if rng.random() < 0.5:
            opts = [d for d in range(4) if (d - model.dir) % 4 != 2]
            req = rng.choice(opts)
            m.w.press_button(m.buttons['NESW'[req]])
        prev_board, prev_len, prev_food, prev_dead = model.board(), model.length, model.food, model.dead
        edge = wait_step(m, p1)
        if not edge:
            print('clock stopped: no P1 edge'); ok = False
            break
        if model.food is None:                 # eaten last step: the machine drew a new one
            model.food = food_pos(m)
            prev_food = model.food
        model.step(req, next_food=None)
        got = board(m); dead = dead_lamp(m)
        checks = {
            'head': (head_pos(m), model.body[0]),
            'tail': (tail_pos(m), model.body[-1]),
            'board': (sorted(got), sorted(prev_board)),
            'len': (length(m), prev_len),
            'food': (food_pos(m), prev_food),
            'dead': (dead, prev_dead),
        }
        if model.dead:                          # the head/tail regs may have stepped into the wall
            checks.pop('head'); checks.pop('tail')
        bad = [k for k, (a, b) in checks.items() if a != b]
        status = 'ok' if not bad else 'MISMATCH ' + ' '.join(f'{k}: got {checks[k][0]} want {checks[k][1]}' for k in bad)
        if bad: ok = False
        print(f'step {s + 1:2d} req={req} head {head_pos(m)} tail {tail_pos(m)} len {length(m)} food {food_pos(m)} '
              f'board {sorted(got)} dead {dead} : {status}', flush=True)
        if model.dead:
            # DEAD gates the clock off; the train may run one more cycle first
            for _ in range(3):
                if not wait_step(m, p1):
                    break
            else:
                print('clock still running after death'); ok = False
            if not dead_lamp(m):
                print('DEAD lamp not lit'); ok = False
            break
    print('PASS' if ok else 'FAIL')
    return ok


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('steps', type=int, nargs='?', default=12)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--pickle', default=None, help='raw Machine pickle (skips the build)')
    a = ap.parse_args()
    main(a.steps, a.seed, a.pickle)
