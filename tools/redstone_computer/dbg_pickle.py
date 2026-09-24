"""Debug helpers on a pickled initialised machine."""
import pickle, sys
sys.setrecursionlimit(100000)
from build import Y
from play_sim import board, head_pos, food_pos, tail_pos, length, wait_step
from pcb import UP, MID

def load(path):
    return pickle.load(open(path, 'rb'))

def ci(w, p):
    c = w.get(p)
    if c is None: return '.'
    if c.kind == 'wire': return format(c.power, 'x')
    if c.kind == 'repeater': return 'R' if c.powered else 'r'
    return {'solid': '#', 'glow': 'g', 'wtorch': 'W' if c.lit else 'w', 'torch': 'T' if c.lit else 't', 'lamp': 'o'}.get(c.kind, c.kind[:2])

def clock_trace(m, ticks=900):
    w = m.w; f = m.f
    clk = f.lanes['CLK']; g = f.lanes['G']; p1 = m.P[0]; dfb = f.lanes['DEADFB']; clkl = f.lanes['CLK_L']
    hist = {k: '' for k in ('clk', 'clkL', 'dfb', 'g', 'p1')}
    for t in range(ticks):
        w.tick()
        hist['clk'] += '1' if w.get((clk.x, Y, clk.z0)).power else '0'
        hist['clkL'] += '1' if w.get((clkl.x, Y, clkl.z0)).power else '0'
        hist['dfb'] += '1' if w.get((dfb.x, Y, dfb.z1)).power else '0'
        hist['g'] += '1' if w.get((g.x, Y, g.z1)).power else '0'
        hist['p1'] += '1' if w.get((p1.x, Y, p1.z0)).power else '0'
    for k, v in hist.items(): print(k.ljust(5), v[:450])

if __name__ == '__main__':
    m = load(sys.argv[1])
    w = m.w
    print('head', head_pos(m), 'tail', tail_pos(m), 'food', food_pos(m), 'L', length(m), 'board', sorted(board(m)), 'dead', w.lamp((m.dead_q.x, Y - 1, m.dead_q.z0)))
    clock_trace(m)
    print('after: head', head_pos(m), 'board', sorted(board(m)), 'dead', w.lamp((m.dead_q.x, Y - 1, m.dead_q.z0)))
