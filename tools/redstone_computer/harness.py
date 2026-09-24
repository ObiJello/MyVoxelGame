"""Runs the real engine on a fresh copy of the machine, sets the levers with
/setblock, lets it settle, and diffs the saved world against the simulator.

    python3 harness.py A B CIN          e.g.  python3 harness.py 15 15 0
"""
import os, subprocess, sys, time, shutil, gzip, zlib, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_world import machine, write
import writer
from nbtread import read_region_chunk, Tag

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAME = os.path.join(ROOT, 'cmake-build-release/bin/MyVoxelGame.app/Contents/MacOS/MyVoxelGame')
SAVES = os.path.expanduser('~/Library/Application Support/obeycraft/saves')
NAME = 'RC Harness'


def lever_cmd(p, on):
    x, y, z = p[0], p[1] + writer.SURFACE_Y, p[2]
    return f'/setblock {x} {y} {z} minecraft:lever[face=floor,facing=south,powered={"true" if on else "false"}]'


def read_world_states(folder, positions):
    """positions: iterable of world (x,y,z). Returns dict pos -> (name, props)."""
    by_chunk = {}
    for p in positions:
        by_chunk.setdefault((p[0] >> 4, p[2] >> 4), []).append(p)
    out = {}
    cache = {}
    for (cx, cz), plist in by_chunk.items():
        path = os.path.join(folder, 'region', f'r.{cx >> 5}.{cz >> 5}.mca')
        if path not in cache:
            cache[path] = open(path, 'rb').read()
        b = cache[path]
        i = 4 * ((cx & 31) + (cz & 31) * 32)
        off = int.from_bytes(b[i:i + 3], 'big') * 4096
        if off == 0:
            continue
        ln = int.from_bytes(b[off:off + 4], 'big'); comp = b[off + 4]
        raw = zlib.decompress(b[off + 5:off + 4 + ln]) if comp == 2 else gzip.decompress(b[off + 5:off + 4 + ln])
        from nbtread import parse
        nm, root = parse(raw)
        secs = {}
        for s in root.v['sections'].v[1]:
            secs[s['Y'].v] = s
        for p in plist:
            x, y, z = p
            s = secs.get(y >> 4)
            if s is None:
                out[p] = ('missing', {}); continue
            bs = s['block_states'].v
            pal = [(e['Name'].v, {k: v.v for k, v in e.get('Properties', Tag(10, {})).v.items()})
                   for e in bs['palette'].v[1]]
            if 'data' not in bs:
                out[p] = pal[0]; continue
            data = bs['data'].v
            bits = max(4, (len(pal) - 1).bit_length()); per = 64 // bits
            idx = ((y & 15) * 16 + (z & 15)) * 16 + (x & 15)
            l = data[idx // per] & ((1 << 64) - 1)
            out[p] = pal[(l >> ((idx % per) * bits)) & ((1 << bits) - 1)]
    return out


def expected_state(w, pos, blk):
    return writer.block_state(w, pos, blk)


def main():
    a, c, cin = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    quit_after = float(sys.argv[4]) if len(sys.argv) > 4 else 60
    folder = os.path.join(SAVES, NAME)
    shutil.rmtree(folder, ignore_errors=True)
    write(NAME)

    b, pins, info = machine(); w = b.w
    cmds = ['--exec-at', '12', '/tp 120 -30 90']
    t = 20
    levers = [(pins['A'][i], (a >> i) & 1) for i in range(4)] + \
             [(pins['B'][i], (c >> i) & 1) for i in range(4)] + [(pins['Cin'], cin)]
    for p, on in levers:
        if on:
            cmds += ['--exec-at', str(t), lever_cmd(p, 1)]
            t += 0.5
    args = [GAME, '--world', NAME, '--env', 'OBEY_HOST_PORT=25599',
            '--quit-after', str(quit_after)] + cmds
    print('launching', ' '.join(args[1:]))
    t0 = time.time()
    r = subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=quit_after + 120)
    print('game exited', r.returncode, 'after %.0fs' % (time.time() - t0))

    # simulate the same inputs
    for p, on in levers:
        w.set_lever(p, on)
    w.settle()
    positions = {}
    for p, blk in w.blocks.items():
        positions[(p[0], p[1] + writer.SURFACE_Y, p[2])] = (p, blk)
    actual = read_world_states(folder, positions.keys())
    kinds = {}
    mism = []
    for wp, (p, blk) in positions.items():
        exp = expected_state(w, p, blk)
        got = actual.get(wp, ('missing', {}))
        kinds.setdefault(blk.kind, [0, 0])[0] += 1
        same = exp[0] == got[0] and all(got[1].get(k) == v for k, v in exp[1].items())
        if not same:
            kinds[blk.kind][1] += 1
            mism.append((wp, blk.kind, exp, got))
    print('per kind (total, mismatched):', kinds)
    mism.sort(key=lambda m: (m[0][2], m[0][0], m[0][1]))
    for m in mism[:40]:
        print(m)
    # summary of outputs
    def lit(p):
        wp = (p[0], p[1] + writer.SURFACE_Y, p[2])
        return actual.get(wp, ('?', {}))[1].get('lit')
    print('engine sum lamps', [lit(pins['S'][i]) for i in range(4)], 'cout', lit(pins['Cout']))
    print('sim    sum lamps', [w.lamp(pins['S'][i]) for i in range(4)], 'cout', w.lamp(pins['Cout']))
    print('engine digit', {s: lit(info['lamps'][s]) for s in 'abcdefg'})


if __name__ == '__main__':
    main()
