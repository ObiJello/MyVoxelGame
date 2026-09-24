"""Build Snake v3 and write it as an ObeyCraft world.

    python3 write_snake_world.py [--name "Redstone Snake"] [--pickle m3_raw.pkl]

The machine is written in its initial, paused state. You spawn on the
viewing deck facing the lamp screen; the console desk in front of you has
the start lever (ON = paused, flip it OFF to play), four buttons in a
D-pad (far = up, near = down, right, left) and, in the floor west of the
desk, the lamp that lights when the snake dies (reload the world to play
again).
"""
import os, sys, pickle
from build import Y
import writer
from snake3 import Machine3, PZ, BX, BZ
import play2

NAME = 'Redstone Snake'


def main():
    args = sys.argv[1:]
    name = NAME
    if '--name' in args:
        name = args[args.index('--name') + 1]
    pk = None
    if '--pickle' in args:
        pk = args[args.index('--pickle') + 1]
    sys.setrecursionlimit(100000)
    m = pickle.load(open(pk, 'rb')) if pk else Machine3(plus='--plus' in args)
    play2.init_state(m)                      # paused, registers and board forced, everything consistent
    w = m.w
    import fastsim; fastsim.attach(w)        # the C engine for the ticks below
    # the RNG runs freely in the world; the oscillator's own pause lever is off, only the
    # platform lever holds the clock
    for lv in m.pause:
        if lv != (m.pause[1]):
            w.toggle_lever(lv, False)
    w.toggle_lever(m.pause[1], True)
    # a restart machine (m.init) resets while paused: let the counters walk to their start values
    # before the state is written, so the world opens holding the initial position
    w.tick(1500 if getattr(m, 'init', None) else 200)
    print('paused state:', end=' '); play2.report(m, 'world')
    saves = os.path.expanduser('~/Library/Application Support/obeycraft/saves')
    folder = os.path.join(saves, name)
    tmpl = os.path.join(saves, 'grass flat (2)')
    sx, sy, sz = m.spawn                                 # on the deck, east of the console
    n = writer.write_world(w, folder, name, (sx, writer.SURFACE_Y + sy, sz),
                           os.path.join(tmpl, 'level.dat'),
                           os.path.join(tmpl, 'data', 'obeycraft.json'))
    print('wrote', n, 'chunks,', len(w.blocks), 'blocks to', folder)
    print('controls: buttons N/E/S/W at', [m.buttons[d] for d in 'NESW'], 'start lever at', m.pause[1],
          'death lamp at', m.dead_lamp, 'spawn', m.spawn, '(sim coordinates; world y = sim y +', writer.SURFACE_Y, ')')


if __name__ == '__main__':
    main()
