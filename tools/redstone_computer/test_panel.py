"""Isolation test of panel.py: lever-driven matrix-side lines, every pixel
set / checked / erased, the food shown, wrong line pairs ignored, CLEAR."""
import sys; sys.path.insert(0, '/Users/obey/Desktop/MyVoxelGame/tools/redstone_computer')
import build
from build import Builder
from redtick import TickWorld
import fastsim
from panel import panel, N
build.set_plus(True, True)
w = TickWorld(); w.no_decay = True
b = Builder(w)
ports = panel(b, 0, 16)
lev = {}
for name in ('HX', 'TX', 'FX'):
    for i, (x, y, z) in enumerate(ports[name]):
        for k in range(0, 4): b.wire(x, 0, z - k)
        b.lever(x, 0, z - 4, 'north'); lev[(name, i)] = (x, 0, z - 4)
for name in ('HYS', 'HYC', 'TYE', 'FY'):
    for j, (x, y, z) in enumerate(ports[name]):
        for k in range(0, 4): b.glow(x - k, 1, z); b.wire(x - k, 2, z)
        b.glow(x - 4, 1, z); b.lever(x - 4, 2, z, 'north'); lev[(name, j)] = (x - 4, 2, z)
cx, cy, cz = ports['CLEAR']
for k in (1, 2): b.glow(cx, cy - 1, cz + k); b.wire(cx, cy, cz + k)
b.glow(cx, cy - 1, cz + 3); b.lever(cx, cy, cz + 3, 'north'); lev['CLEAR'] = (cx, cy, cz + 3)
ox, oy, oz = ports['COL']; b.glow(ox - 1, oy - 1, oz); b.wire(ox - 1, oy, oz); b.lamp(ox - 2, oy, oz)   # a straight cell past the merge corner, then the lamp
px_, py_, pz_ = ports['PRESENT']
for k in (1, 2): b.glow(px_ - k, py_ - 1, pz_); b.wire(px_ - k, py_, pz_)
b.glow(px_ - 3, py_ - 1, pz_); b.lever(px_ - 3, py_, pz_, 'north'); lev['PRESENT'] = (px_ - 3, py_, pz_)
assert not b.collisions, b.collisions[:5]
for k in lev: w.get(lev[k]).powered = (k not in ('CLEAR', 'PRESENT'))
w.settle(); w.boot(); w.run_until_idle()
if '--slow' not in sys.argv: fastsim.attach(w)
def sel(name, i, on): w.toggle_lever(lev[(name, i)], not on)
def pix(): return {(i, j): w.display_bits(ports['pixel'][(i, j)]) for i in range(N) for j in range(N)}
def wall(): return {(i, j): w.display_bits(ports['wall'][(i, j)]) for i in range(N) for j in range(N)}
def present():
    """Strobe the frame buffer: every wall face must then equal its pixel."""
    w.toggle_lever(lev['PRESENT'], True); w.tick(4); w.toggle_lever(lev['PRESENT'], False); w.tick(4)
    assert wall() == pix(), ('wall != pixels after PRESENT', {k: (wall()[k], pix()[k]) for k in pix() if wall()[k] != pix()[k]})
def only(expect):
    got = {k: v for k, v in pix().items() if v}
    return got == expect, (got if got != expect else 'ok')
ok = True
def chk(tag, r):
    global ok
    ok &= r[0]; print(tag, r[1])
sel('HYS', 5, True); sel('HX', 3, True); w.tick(4); chk('set (3,5)', only({(3, 5): 2}))
print('wall before PRESENT shows nothing:', all(v == 0 for v in wall().values())); ok &= all(v == 0 for v in wall().values())
present(); print('wall after PRESENT matches')
sel('HYS', 5, False); sel('HX', 3, False); w.tick(4); chk('latched', only({(3, 5): 2}))
sel('TYE', 5, True); sel('HX', 3, True); w.tick(4); chk('TYE+HX', only({(3, 5): 2})); sel('TYE', 5, False); sel('HX', 3, False)
sel('HYS', 5, True); sel('TX', 3, True); w.tick(4); chk('HYS+TX', only({(3, 5): 2})); sel('HYS', 5, False); sel('TX', 3, False)
sel('HYC', 5, True); sel('HX', 3, True); w.tick(4); lit = w.lamp((ox - 2, oy, oz)); ok &= lit; print('check hit -> lamp', lit)
sel('HX', 3, False); sel('HX', 4, True); w.tick(6); lit = w.lamp((ox - 2, oy, oz)); ok &= not lit; print('check miss -> lamp', lit); sel('HX', 4, False); sel('HYC', 5, False)
sel('FY', 2, True); sel('FX', 6, True); w.tick(4); chk('food', only({(3, 5): 2, (6, 2): 1}))
sel('FY', 2, False); sel('FX', 6, False); w.tick(4); chk('food off', only({(3, 5): 2}))
sel('TYE', 5, True); sel('TX', 3, True); w.tick(4); chk('erase', only({})); sel('TYE', 5, False); sel('TX', 3, False)
print('wall still shows the old frame:', wall()[(3, 5)] == 2); ok &= wall()[(3, 5)] == 2
present(); print('wall cleared after PRESENT:', wall()[(3, 5)] == 0); ok &= wall()[(3, 5)] == 0
for j in range(N): sel('HYS', j, True)
for i in range(N): sel('HX', i, True)
w.tick(6); chk('set all', only({(i, j): 2 for i in range(N) for j in range(N)}))
for j in range(N): sel('HYS', j, False)
for i in range(N): sel('HX', i, False)
present()
w.toggle_lever(lev['CLEAR'], True); w.tick(4); chk('clear all', only({})); w.toggle_lever(lev['CLEAR'], False)
present()
# every pixel individually: set, check, erase
bad = 0
for i in range(N):
    for j in range(N):
        sel('HYS', j, True); sel('HX', i, True); w.tick(4); r = only({(i, j): 2}); bad += not r[0]
        sel('HYS', j, False); sel('HYC', j, True); w.tick(4); bad += not w.lamp((ox - 2, oy, oz))
        sel('HYC', j, False); sel('HX', i, False); sel('TYE', j, True); sel('TX', i, True); w.tick(4); r = only({}); bad += not r[0]
        sel('TYE', j, False); sel('TX', i, False)
print('per-pixel failures', bad); ok &= bad == 0
print('PANEL', 'PASS' if ok else 'FAIL')
