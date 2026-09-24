from build import Builder, Y, dump
from redtick import TickWorld
from pcb import Pcb, UP, MID
from gridcell import cell, through_lanes, through_rows, W

def build():
    b = Builder(TickWorld()); p = Pcb(b); w = b.w
    cx, cz = 40, 40
    # lane sources: levers north of the cell feeding lanes at x=40,43,46
    for lx in (0, 3, 6):
        b.lever(cx + lx, Y, cz - 3, 'south'); b.wire(cx + lx, Y, cz - 2)
    through_lanes(b, cx, cz, cz + W)
    # row sources: levers west, each on a floor stub with a via up to the row start
    for rz in (0, 8, 16, 24):
        b.lever(cx - 12, Y, cz + rz, 'east'); b.wire(cx - 11, Y, cz + rz); b.wire(cx - 10, Y, cz + rz)
        p.via_up(cx - 10, cz + rz)          # -> (cx-6, rz) then the row from cx-1
        for x in range(cx - 6, cx - 1): p.upper_wire(x, cz + rz)
    through_rows(p, cx, cx + W, cz)
    info = cell(b, cx, cz)
    b.lamp(cx + 27, Y - 1, cz + W + 1)      # DEAD collector lamp at the lane end
    return b, w, cx, cz, info

def test():
    b, w, cx, cz, info = build()
    L = {'HX': (cx, Y, cz - 3), 'TX': (cx + 3, Y, cz - 3), 'FX': (cx + 6, Y, cz - 3),
         'HYS': (cx - 12, Y, cz), 'HYC': (cx - 12, Y, cz + 8), 'TYE': (cx - 12, Y, cz + 16), 'FY': (cx - 12, Y, cz + 24)}
    # all active-low lines start HIGH (lever on)
    for k in L: w.toggle_lever(L[k], True)
    w.boot(); w.run_until_idle()
    body = lambda: w.lamp(info['body_lamp']); food = lambda: w.lamp(info['food_lamp'])
    dead = lambda: w.lamp((cx + 27, Y - 1, cz + W + 1))
    print('initial body', body(), 'food', food(), 'dead', dead())
    # make sure the latch starts reset: pulse RESET (TX low + TYE low)
    w.toggle_lever(L['TX'], False); w.toggle_lever(L['TYE'], False); w.run_until_idle()
    w.toggle_lever(L['TYE'], True); w.toggle_lever(L['TX'], True); w.run_until_idle()
    assert not body(), 'reset'
    # food: FX low & FY low
    w.toggle_lever(L['FX'], False); w.toggle_lever(L['FY'], False); w.run_until_idle()
    assert food() and not body(), 'food'
    w.toggle_lever(L['FX'], True); w.toggle_lever(L['FY'], True); w.run_until_idle(); assert not food()
    # check (head here, PCHECK) while empty: no death
    w.toggle_lever(L['HX'], False); w.toggle_lever(L['HYC'], False); w.run_until_idle()
    assert not dead(), 'check on empty cell must not kill'
    w.toggle_lever(L['HYC'], True); w.run_until_idle()
    # set: HX low (still) & HYS low pulse
    w.toggle_lever(L['HYS'], False); w.run_until_idle(); w.toggle_lever(L['HYS'], True); w.run_until_idle()
    assert body(), 'set'
    # check again while set: death
    w.toggle_lever(L['HYC'], False); w.run_until_idle()
    assert dead(), 'collision'
    w.toggle_lever(L['HYC'], True); w.run_until_idle(); assert not dead()
    # reset
    w.toggle_lever(L['TX'], False); w.toggle_lever(L['TYE'], False); w.run_until_idle()
    assert not body(), 'reset 2'
    w.toggle_lever(L['TYE'], True); w.toggle_lever(L['TX'], True); w.run_until_idle()
    print('ok gridcell (blocks %d)' % len(w.blocks))

if __name__ == '__main__':
    test()
