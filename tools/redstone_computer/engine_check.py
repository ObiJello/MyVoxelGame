"""Generic engine-vs-simulator check for clocked circuits.

    check(name, world, script, quit_after)

`world` is a booted TickWorld; `script` is a list of (seconds, action) where
action is ('lever', pos, on) or ('button', pos). The engine gets the same
events as /setblock commands (a button is released one second later), then
quits and saves; the simulator gets them at the same tick offsets and runs to
idle. Every block of the two worlds is compared.
"""
import os, subprocess, shutil, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import writer
from harness import GAME, SAVES, read_world_states, lever_cmd

T0 = 20.0          # first event, seconds after session start (world loaded by then)


def button_cmd(p, on):
    x, y, z = p[0], p[1] + writer.SURFACE_Y, p[2]
    return f'/setblock {x} {y} {z} minecraft:stone_button[face=floor,facing=south,powered={"true" if on else "false"}]'


def check(name, w, script, quit_after=None, tp=(20, -50, 20), verbose=True):
    folder = os.path.join(SAVES, name)
    shutil.rmtree(folder, ignore_errors=True)
    tmpl = os.path.join(SAVES, 'grass flat (2)')
    writer.write_world(w, folder, name, (tp[0], writer.SURFACE_Y, tp[2]),
                       os.path.join(tmpl, 'level.dat'), os.path.join(tmpl, 'data', 'obeycraft.json'))
    cmds = ['--exec-at', '12', f'/tp {tp[0]} {tp[1]} {tp[2]}']
    last = T0
    for sec, act in script:
        t = T0 + sec
        if act[0] == 'lever':
            cmds += ['--exec-at', f'{t:.2f}', lever_cmd(act[1], act[2])]
        else:
            cmds += ['--exec-at', f'{t:.2f}', button_cmd(act[1], True)]
            cmds += ['--exec-at', f'{t + 1.0:.2f}', button_cmd(act[1], False)]
            t += 1.0
        last = max(last, t)
    quit_after = quit_after or (last + 15)
    args = [GAME, '--world', name, '--env', 'OBEY_HOST_PORT=25599', '--quit-after', str(quit_after)] + cmds
    t0 = time.time()
    r = subprocess.run(args, cwd=os.path.dirname(os.path.dirname(os.path.dirname(GAME))),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=quit_after + 120)
    if verbose:
        print(f'engine exited {r.returncode} after {time.time() - t0:.0f}s')

    # the same events in the simulator
    now = 0.0
    for sec, act in script:
        dt = sec - now
        if dt > 0:
            w.tick(int(round(dt * 20)))
        now = sec
        if act[0] == 'lever':
            w.toggle_lever(act[1], act[2])
        else:
            w.press_button(act[1])
    w.run_until_idle()

    positions = {(p[0], p[1] + writer.SURFACE_Y, p[2]): (p, blk) for p, blk in w.blocks.items()}
    actual = read_world_states(folder, positions.keys())
    kinds = {}; mism = []
    for wp, (p, blk) in positions.items():
        exp = writer.block_state(w, p, blk); got = actual.get(wp, ('missing', {}))
        kinds.setdefault(blk.kind, [0, 0])[0] += 1
        same = exp[0] == got[0] and all(got[1].get(k) == v for k, v in exp[1].items())
        if not same:
            kinds[blk.kind][1] += 1; mism.append((wp, blk.kind, exp[1], got[1]))
    if verbose:
        print('per kind (total, mismatched):', kinds)
        for m in sorted(mism)[:25]:
            print('  ', m)
    return mism, actual
