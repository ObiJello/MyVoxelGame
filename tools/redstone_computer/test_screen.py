"""Static screen test: force board patterns, settle, compare the wall.

For each pattern: every pixel's body torches are pinned (like init_state), the
food registers are pinned to one cell; after a pinned settle + run, each wall
pixel's 12 lamps must show exactly the plus (body), the L (food), both or
nothing. Also checks the deck's death lamp follows the DEAD latch.
"""
import sys, time
sys.path.insert(0, '/Users/obey/Desktop/MyVoxelGame/tools/redstone_computer')
sys.setrecursionlimit(100000)
from build import Y
from play2 import load, init_state, pixel_torches, set_repeater, board, foods
from screen import plane, height, WX, N

S = sys.argv[1] if len(sys.argv) > 1 else '.'
m = load(S + '/m3_raw.pkl'); w = m.w
init_state(m)                      # pause levers on, head (3,3), food (5,3)

def wall_pixel(i, j):
    P, h = plane(i), height(j)
    return {(z - (P - 2), y - h): w.lamp((WX, y, z)) for z in range(P - 2, P + 2) for y in range(h, h + 3)}

PLUS = {(2, 1), (1, 1), (3, 1), (2, 0), (2, 2)}
ELL = {(0, 2), (1, 2), (0, 1)}

def expect(body, food):
    return (PLUS if body else set()) | (ELL if food else set())

def rs(name_q, on):
    """Force an RS latch (rslatch.rs_latch geometry: torches at xa+1 / xa+7) and return its torches."""
    lane = m.f.lanes[name_q]; xa = lane.x - 9; z = lane.z0 - 1
    w.get((xa + 1, Y, z)).lit = not on; w.get((xa + 7, Y, z)).lit = bool(on)
    return [(xa + 1, Y, z), (xa + 7, Y, z)]

# a restart machine resets while its deck lever is on (registers forced, board cleared, DEAD
# held): release that lever so the reset ends (the oscillator's own lever still holds the clock)
EXTRA_PINS = []
if 'RSTHQ' in m.f.lanes:
    w.toggle_lever(m.pause[1], False); w.tick(400)
    print('restart machine: deck lever released, reset latch', w.get((m.f.lanes['RSTHQ'].x, Y, m.f.lanes['RSTHQ'].z0)).power > 0)

def set_pattern(cells, food):
    pins = list(EXTRA_PINS)
    for i in range(N):
        for j in range(N):
            on = (i, j) in cells
            ta, tb = pixel_torches(m, i, j)
            w.get(ta).lit = not on; w.get(tb).lit = on
            pins += [ta, tb]
    inv = 1 if getattr(m, 'food_inverted', False) else 0
    for k in range(3):
        set_repeater(w, m.regs[f'FX{k}'], ((food[0] >> k) & 1) ^ inv)
        set_repeater(w, m.regs[f'FY{k}'], ((food[1] >> k) & 1) ^ inv)
    pins += list(m.regs.values())
    t0 = time.time()
    w.settle(max_rounds=300, pinned=pins)
    w.boot(); w.run_until_idle(limit=2000)
    return time.time() - t0

ok = True
patterns = [
    ('empty', set(), (2, 6)),
    ('diagonal', {(k, k) for k in range(8)}, (0, 7)),
    ('checker', {(i, j) for i in range(8) for j in range(8) if (i + j) % 2 == 0}, (7, 0)),
    ('full', {(i, j) for i in range(8) for j in range(8)}, (3, 3)),
    ('row3', {(i, 3) for i in range(8)}, (5, 5)),
]
# a restart machine (m.init) is in reset while paused: its food registers are forced to the
# initial food, so every pattern shows that food
for name, cells, food in patterns:
    dt = set_pattern(cells, food)
    b, f = board(m), foods(m)
    bad = []
    for i in range(N):
        for j in range(N):
            got = {k for k, v in wall_pixel(i, j).items() if v}
            want = expect((i, j) in cells, (i, j) == food)
            if got != want:
                bad.append(((i, j), sorted(got), sorted(want)))
    print(f'{name:9s} board ok {sorted(b) == sorted(cells)} foods ok {sorted(f) == [food]} wall bad {len(bad)} ({dt:.0f}s)', flush=True)
    for x in bad[:6]:
        print('   ', x)
    if bad or sorted(b) != sorted(cells) or sorted(f) != [food]:
        ok = False
# death lamp: set the DEAD latch, run, check the deck lamp; clear it, check again
for on in (True, False):
    rs('DEAD', on)
    w.settle(max_rounds=300, pinned=EXTRA_PINS + list(m.regs.values()) + [p for i in range(N) for j in range(N) for p in pixel_torches(m, i, j)])
    w.boot(); w.run_until_idle(limit=2000)
    lit = w.lamp(m.dead_lamp)
    print('DEAD', on, '-> deck lamp', lit)
    if lit != on: ok = False
print('PASS' if ok else 'FAIL')
