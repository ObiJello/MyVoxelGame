"""Static topology check of the screen wiring: every chain's dust must form
its own connected component (using the simulator's connection rules)."""
import sys, pickle
sys.path.insert(0, '/Users/obey/Desktop/MyVoxelGame/tools/redstone_computer')
sys.setrecursionlimit(100000)
from screen import body_cells, food_cells, N
HORIZ = {'north': (0, 0, -1), 'south': (0, 0, 1), 'east': (1, 0, 0), 'west': (-1, 0, 0)}

DELTA = {'north': (0, 0, -1), 'south': (0, 0, 1), 'east': (1, 0, 0), 'west': (-1, 0, 0)}
OPPO = {'north': 'south', 'south': 'north', 'east': 'west', 'west': 'east'}

def dust_neighbours(w, pos):
    """cells this dust/repeater connects to (both directions are checked by the caller's BFS)."""
    out = []
    x, y, z = pos
    b = w.blocks[pos]
    if b.kind == 'repeater':
        for d in (b.facing, OPPO[b.facing]):
            dx, dy, dz = DELTA[d]
            n = (x + dx, y, z + dz)
            if w.blocks.get(n) is not None and w.blocks[n].kind in ('wire', 'repeater'):
                out.append(n)
        return out
    sides = w.wire_sides(pos)
    for d, (dx, dy, dz) in HORIZ.items():
        s = sides[d]
        if s == 'none':
            continue
        n = (x + dx, y, z + dz)
        nb = w.blocks.get(n)
        if s == 'up':
            up = (x + dx, y + 1, z + dz)
            if w.blocks.get(up) is not None and w.blocks[up].kind == 'wire':
                out.append(up)
        if nb is not None and nb.kind in ('wire', 'repeater'):
            out.append(n)
        elif not w.is_conductor(n):
            dn = (x + dx, y - 1, z + dz)
            if w.blocks.get(dn) is not None and w.blocks[dn].kind == 'wire':
                out.append(dn)
    return out

def check(m, verbose=True):
    w = m.w
    label = {}
    for i in range(N):
        for j in range(N):
            for kind, fn in (('B', body_cells), ('F', food_cells)):
                cells, link = fn(i, j)[:2]
                for c in cells + link:
                    label[c] = (kind, i, j)
    bad = []
    seen = set()
    for i in range(N):
        for j in range(N):
            for kind, fn in (('B', body_cells), ('F', food_cells)):
                cells, link = fn(i, j)[:2]
                for start in (cells[0], link[0]):
                    if start in seen: continue
                    stack = [start]; seen.add(start)
                    while stack:
                        p = stack.pop()
                        for q in dust_neighbours(w, p):
                            if label.get(q) != (kind, i, j):
                                bad.append((p, label.get(p), q, label.get(q)))
                                continue
                            if q not in seen:
                                seen.add(q); stack.append(q)
    if verbose:
        print(len(bad), 'foreign connections')
        shown = set()
        for p, lp, q, lq in bad:
            key = (lp, lq)
            if key in shown: continue
            shown.add(key); print('  ', p, lp, '<->', q, lq)
    return bad

if __name__ == '__main__':
    S = sys.argv[1] if len(sys.argv) > 1 else '.'
    m = pickle.load(open(S + '/m3_raw.pkl', 'rb'))
    check(m)
