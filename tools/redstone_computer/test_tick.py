from build import Builder, Y
from redtick import TickWorld

def mk():
    return Builder(TickWorld())

def test_torch_delay():
    b = mk(); w = b.w
    b.lever(0, Y, 0); b.line(1, 0, 2, 0); b.solid(3, Y, 0); b.wtorch(4, Y, 0, 'east'); b.lamp(5, Y, 0)
    w.settle()
    assert w.lamp((5, Y, 0))
    w.toggle_lever((0, Y, 0), True)
    seen = None
    for t in range(1, 10):
        w.tick()
        if not w.get((4, Y, 0)).lit and seen is None:
            seen = t
    assert seen == 2, seen
    # lamp goes off 4 ticks after the torch
    w2 = w; lamp_off = None
    # re-run precisely
    b = mk(); w = b.w
    b.lever(0, Y, 0); b.line(1, 0, 2, 0); b.solid(3, Y, 0); b.wtorch(4, Y, 0, 'east'); b.lamp(5, Y, 0)
    w.settle(); w.toggle_lever((0, Y, 0), True)
    for t in range(1, 12):
        w.tick()
        if not w.lamp((5, Y, 0)) and lamp_off is None:
            lamp_off = t
    assert lamp_off == 6, lamp_off

def test_repeater_delays():
    for d in (1, 2, 3, 4):
        b = mk(); w = b.w
        b.lever(0, Y, 0); b.wire(1, Y, 0); b.repeater(2, Y, 0, 'west', delay=d); b.wire(3, Y, 0)
        w.settle(); w.toggle_lever((0, Y, 0), True)
        on = None
        for t in range(1, 12):
            w.tick()
            if w.get((3, Y, 0)).power and on is None:
                on = t
        assert on == 2 * d, (d, on)
        w.toggle_lever((0, Y, 0), False)
        off = None
        for t in range(1, 12):
            w.tick()
            if not w.get((3, Y, 0)).power and off is None:
                off = t
        assert off == 2 * d, (d, off)

def test_repeater_min_pulse():
    # a 1-tick input pulse through a 4-tick repeater comes out 4 ticks long
    b = mk(); w = b.w
    b.lever(0, Y, 0); b.wire(1, Y, 0); b.repeater(2, Y, 0, 'west', delay=2); b.wire(3, Y, 0)
    w.settle(); w.toggle_lever((0, Y, 0), True); w.tick(); w.toggle_lever((0, Y, 0), False)
    hist = []
    for t in range(12):
        w.tick(); hist.append(int(w.get((3, Y, 0)).power > 0))
    assert sum(hist) == 4, hist

def test_torch_clock():
    # Three inverter stages in a ring. A torch strongly powers only the block
    # ABOVE it, so a stage is torch -> wire -> block -> torch (the wire gives
    # the block strong power). The return wire needs a repeater to reach:
    # period = 3 stages * 2 ticks * 2 phases + repeater 2 * 2 = 16.
    b = mk(); w = b.w
    for i in range(3):
        x = i * 3
        b.solid(x, Y, 0); b.wtorch(x + 1, Y, 0, 'east'); b.wire(x + 2, Y, 0)
    b.line(8, 1, 8, 2); b.line(-2, 2, 7, 2)
    b.repeater(-2, Y, 1, 'south'); b.wire(-2, Y, 0); b.wire(-1, Y, 0)
    w.boot()
    w.tick(60)
    hist = []
    for t in range(80):
        w.tick(); hist.append(int(w.get((1, Y, 0)).lit))
    edges = [i for i in range(1, len(hist)) if hist[i] and not hist[i - 1]]
    periods = [b_ - a_ for a_, b_ in zip(edges, edges[1:])]
    assert periods and all(p == 16 for p in periods), (hist, periods)

def test_lock_latch():
    # data lever -> repeater R (east); lock lever -> repeater L facing north into R's south side
    b = mk(); w = b.w
    b.lever(0, Y, 0); b.wire(1, Y, 0); b.repeater(2, Y, 0, 'west'); b.wire(3, Y, 0)
    b.lever(2, Y, 3); b.wire(2, Y, 2); b.repeater(2, Y, 1, 'south')
    w.settle()
    w.toggle_lever((2, Y, 3), True); w.run_until_idle()      # lock while R is off
    w.toggle_lever((0, Y, 0), True); w.run_until_idle()
    assert w.get((3, Y, 0)).power == 0                       # held
    w.toggle_lever((2, Y, 3), False); w.run_until_idle()     # unlock
    assert w.get((3, Y, 0)).power == 15                      # follows data
    w.toggle_lever((2, Y, 3), True); w.run_until_idle()
    w.toggle_lever((0, Y, 0), False); w.run_until_idle()
    assert w.get((3, Y, 0)).power == 15                      # held on

for f in (test_torch_delay, test_repeater_delays, test_repeater_min_pulse, test_torch_clock, test_lock_latch):
    f(); print('ok', f.__name__)
