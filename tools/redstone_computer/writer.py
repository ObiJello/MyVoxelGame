"""Writes the simulated circuit into an ObeyCraft/Minecraft world folder.

Anvil layout the engine reads today (DataVersion 4764, old folder layout):
  <world>/level.dat, data/obeycraft.json, region/r.X.Z.mca
Chunks carry the default flat terrain (bedrock, 2 dirt, grass at y=-61) plus
the circuit, whose simulator level Y=0 maps to world y = SURFACE_Y.
"""
import glob, gzip, json, os, struct, time, zlib, random
from redsim import DIRS

SURFACE_Y = -60        # first air block above the flat grass; sim Y=0 lives here
DATA_VERSION = 4764
MIN_Y = -64
SECTIONS = 24

# ── NBT writer (tag types: 1 byte 2 short 3 int 4 long 5 float 6 double 7 bytes 8 str 9 list 10 compound 11 ints 12 longs)
def _name(s):
    e = s.encode('utf8'); return struct.pack('>H', len(e)) + e

def _payload(t, v):
    if t == 1: return struct.pack('>b', v)
    if t == 2: return struct.pack('>h', v)
    if t == 3: return struct.pack('>i', v)
    if t == 4: return struct.pack('>q', v)
    if t == 5: return struct.pack('>f', v)
    if t == 6: return struct.pack('>d', v)
    if t == 8: return _name(v)
    if t == 9:
        et, items = v
        return struct.pack('>bi', et, len(items)) + b''.join(_payload(et, i) for i in items)
    if t == 10:
        out = b''
        for k, (tt, vv) in v.items():
            out += struct.pack('>b', tt) + _name(k) + _payload(tt, vv)
        return out + b'\x00'
    if t == 11: return struct.pack('>i', len(v)) + struct.pack('>%di' % len(v), *v)
    if t == 12: return struct.pack('>i', len(v)) + struct.pack('>%dq' % len(v), *[x if x < 2**63 else x - 2**64 for x in v])
    raise ValueError(t)

def nbt_root(name, compound):
    return struct.pack('>b', 10) + _name(name) + _payload(10, compound)

def S(v): return (8, v)
def I(v): return (3, v)
def B(v): return (1, int(v))
def L(v): return (4, v)
def C(d): return (10, d)
def LIST(et, items): return (9, (et, items))


# ── block state strings for the simulator's blocks ─────────────────────────
def block_state(world, pos, blk):
    k = blk.kind
    if k == 'solid':
        return blk.name or 'minecraft:smooth_stone', {}
    if k == 'glow':
        return 'minecraft:glowstone', {}
    if k == 'wire':
        sides = world.wire_sides(pos)
        return 'minecraft:redstone_wire', {
            'north': sides['north'], 'east': sides['east'],
            'south': sides['south'], 'west': sides['west'],
            'power': str(blk.power)}
    if k == 'repeater':
        locked = world.repeater_locked_now(pos) if hasattr(world, 'repeater_locked_now') else False
        return 'minecraft:repeater', {'facing': blk.facing, 'delay': str(blk.delay),
                                      'locked': str(locked).lower(), 'powered': str(bool(blk.powered)).lower()}
    if k == 'comparator':
        return 'minecraft:comparator', {'facing': blk.facing, 'mode': blk.mode,
                                        'powered': str(blk.output > 0).lower()}
    if k == 'torch':
        name = 'minecraft:blue_redstone_torch' if getattr(blk, 'instant', False) else 'minecraft:redstone_torch'
        return name, {'lit': str(bool(blk.lit)).lower()}
    if k == 'wtorch':
        name = 'minecraft:blue_redstone_wall_torch' if getattr(blk, 'instant', False) else 'minecraft:redstone_wall_torch'
        return name, {'facing': blk.facing, 'lit': str(bool(blk.lit)).lower()}
    if k == 'lever':
        return 'minecraft:lever', {'face': blk.face, 'facing': blk.facing or 'north',
                                   'powered': str(bool(blk.powered)).lower()}
    if k == 'lamp':
        return 'minecraft:redstone_lamp', {'lit': str(bool(blk.lit)).lower()}
    if k == 'display':
        return 'minecraft:display_block', {'red': str(bool(blk.rgb & 1)).lower(), 'green': str(bool(blk.rgb & 2)).lower(),
                                           'blue': str(bool(blk.rgb & 4)).lower(), 'powered': str(bool(blk.powered)).lower(),
                                           'op_nw': blk.op_nw, 'op_se': blk.op_se}
    if k == 'button':
        return 'minecraft:stone_button', {'face': blk.face, 'facing': blk.facing or 'north',
                                          'powered': str(bool(blk.powered)).lower()}
    raise ValueError(k)


def flat_block(y):
    if y == MIN_Y: return ('minecraft:bedrock', {})
    if y in (MIN_Y + 1, MIN_Y + 2): return ('minecraft:dirt', {})
    if y == MIN_Y + 3: return ('minecraft:grass_block', {'snowy': 'false'})
    return None


def pack_section(states):
    """states: list of 4096 (name, props) in MC's (y, z, x) order -> palette + longs."""
    palette = []; index = {}
    ids = []
    for st in states:
        key = (st[0], tuple(sorted(st[1].items())))
        if key not in index:
            index[key] = len(palette); palette.append(st)
        ids.append(index[key])
    if len(palette) == 1:
        return palette, None
    bits = max(4, (len(palette) - 1).bit_length())
    per = 64 // bits
    longs = []
    for i in range(0, 4096, per):
        v = 0
        for j, idx in enumerate(ids[i:i + per]):
            v |= idx << (j * bits)
        longs.append(v)
    return palette, longs


def build_chunk(cx, cz, cells, block_entities, ticks):
    """cells: dict (x,y,z) -> (name, props) for this chunk only."""
    sections = []
    for s in range(SECTIONS):
        sy = s - 4
        base = sy * 16
        states = []
        for ly in range(16):
            y = base + ly
            for lz in range(16):
                for lx in range(16):
                    st = cells.get((cx * 16 + lx, y, cz * 16 + lz))
                    if st is None:
                        st = flat_block(y) or ('minecraft:air', {})
                    states.append(st)
        palette, longs = pack_section(states)
        pal_tags = []
        for name, props in palette:
            entry = {'Name': S(name)}
            if props:
                entry['Properties'] = C({k: S(v) for k, v in props.items()})
            pal_tags.append(entry)
        bs = {'palette': LIST(10, pal_tags)}
        if longs is not None:
            bs['data'] = (12, longs)
        sections.append({
            'Y': B(sy),
            'block_states': C(bs),
            'biomes': C({'palette': LIST(8, ['minecraft:plains'])}),
        })
    root = {
        'DataVersion': I(DATA_VERSION),
        'xPos': I(cx), 'yPos': I(-4), 'zPos': I(cz),
        'LastUpdate': L(0), 'InhabitedTime': L(0),
        'Status': S('minecraft:full'),
        'sections': LIST(10, sections),
        'block_entities': LIST(10, block_entities) if block_entities else LIST(0, []),
        'block_ticks': LIST(10, ticks) if ticks else LIST(0, []),
        'fluid_ticks': LIST(0, []),
        'PostProcessing': LIST(0, []),
    }
    return nbt_root('', root)


def write_region(path, chunks):
    """chunks: dict (cx, cz) -> nbt bytes, all within one region."""
    header = bytearray(8192)
    body = bytearray()
    sector = 2
    now = int(time.time())
    for (cx, cz), data in chunks.items():
        comp = zlib.compress(data)
        payload = struct.pack('>i', len(comp) + 1) + b'\x02' + comp
        pad = (-len(payload)) % 4096
        payload += b'\x00' * pad
        count = len(payload) // 4096
        i = 4 * ((cx & 31) + (cz & 31) * 32)
        header[i:i + 4] = struct.pack('>I', (sector << 8) | count)
        header[4096 + i:4096 + i + 4] = struct.pack('>I', now)
        body += payload
        sector += count
    with open(path, 'wb') as f:
        f.write(header); f.write(body)


def write_world(world, folder, name, spawn, template_level_dat, template_sidecar):
    """world: settled redsim.World. spawn: (x, y, z) world coordinates."""
    os.makedirs(os.path.join(folder, 'region'), exist_ok=True)
    os.makedirs(os.path.join(folder, 'data'), exist_ok=True)
    for sub in ('DIM-1', 'DIM1'):
        for d in ('region', 'entities', 'poi', 'data'):
            os.makedirs(os.path.join(folder, sub, d), exist_ok=True)
    for d in ('entities', 'poi', 'playerdata'):
        os.makedirs(os.path.join(folder, d), exist_ok=True)
    # a rewritten world keeps its folder: stale player files would put the player back where the
    # previous layout's deck was, so every player starts at this layout's spawn
    for f in glob.glob(os.path.join(folder, 'playerdata', '*.dat*')):
        os.remove(f)

    # cells by chunk
    per_chunk = {}
    bes = {}
    ticks = {}
    for (x, y, z), blk in world.blocks.items():
        wy = y + SURFACE_Y
        key = (x >> 4, z >> 4)
        st = block_state(world, (x, y, z), blk)
        per_chunk.setdefault(key, {})[(x, wy, z)] = st
        if blk.kind == 'comparator':
            bes.setdefault(key, []).append({
                'id': S('minecraft:comparator'), 'x': I(x), 'y': I(wy), 'z': I(z),
                'OutputSignal': I(blk.output), 'keepPacked': B(0)})
    # only the sim's genuinely pending ticks are exported (as MC saves them: 't' is the
    # delay from now, 'p' the priority; list order breaks ties). Giving every component
    # a tick instead is NOT harmless: a repeater's tick always turns it on (MC
    # DiodeBlock.tick), so every idle repeater in the machine would fire a pulse at load.
    pending = sorted(world.pending_ticks() if hasattr(world, 'pending_ticks') else getattr(world, '_heap', []))
    for due, prio, _seq, (x, y, z), kind in pending:
        blk = world.blocks.get((x, y, z))
        if blk is None or blk.kind != kind:
            continue
        st = block_state(world, (x, y, z), blk)
        ticks.setdefault((x >> 4, z >> 4), []).append({
            'i': S(st[0]), 'p': I(prio), 't': I(max(0, due - world.time)),
            'x': I(x), 'y': I(y + SURFACE_Y), 'z': I(z)})
    # every chunk the build touches gets written in full (terrain + build);
    # neighbours are left to the engine's flat generator
    regions = {}
    for key, cells in per_chunk.items():
        cx, cz = key
        regions.setdefault((cx >> 5, cz >> 5), {})[key] = build_chunk(
            cx, cz, cells, bes.get(key, []), ticks.get(key, []))
    for (rx, rz), chunks in regions.items():
        write_region(os.path.join(folder, 'region', f'r.{rx}.{rz}.mca'), chunks)
    # every chunk the build touches is force-loaded (vanilla /forceload, data/chunks.dat:
    # MC ForcedChunksSavedData, ChunkPos.asLong packed), so the machine loads and ticks
    # at any distance without a simulation ring over the land around it
    forced = sorted(per_chunk, key=lambda c: (c[1], c[0]))

    # level.dat from the template, renamed and respawned
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from nbtread import parse, serialize, Tag
    nm, root = parse(gzip.open(template_level_dat).read())
    data = root.v['Data'].v
    data['LevelName'] = Tag(8, name)
    data['LastPlayed'] = Tag(4, int(time.time() * 1000))
    data['Time'] = Tag(4, 0); data['DayTime'] = Tag(4, 6000)
    data['GameType'] = Tag(3, 1)
    sx, sy, sz = spawn
    data['SpawnX'] = Tag(3, sx); data['SpawnY'] = Tag(3, sy); data['SpawnZ'] = Tag(3, sz)
    data['spawn'].v['pos'] = Tag(11, [sx, sy, sz])
    data['spawn'].v['yaw'] = Tag(5, 90.0); data['SpawnAngle'] = Tag(5, 90.0)      # facing west, over the panel
    data['WorldGenSettings'].v['seed'] = Tag(4, random.getrandbits(48))
    # older templates named a multi_noise "minecraft:end" preset, which does
    # not exist; the End's biome source is TheEndBiomeSource (see LevelDat.cpp)
    end = data['WorldGenSettings'].v['dimensions'].v['minecraft:the_end'].v['generator'].v
    end['biome_source'] = Tag(10, {'type': Tag(8, 'minecraft:the_end')})
    # the redstone_plus engine rule lives in the obeycraft compound (a key Minecraft does
    # not know inside game_rules would fail its decode); a no-decay machine needs it on
    data.setdefault('obeycraft', Tag(10, {})).v['redstone_plus'] = Tag(1, 1 if getattr(world, 'no_decay', False) else 0)
    with open(os.path.join(folder, 'level.dat'), 'wb') as f:
        f.write(gzip.compress(serialize(nm, root)))
    packed = [((cx & 0xFFFFFFFF) | ((cz & 0xFFFFFFFF) << 32)) for cx, cz in forced]
    packed = [v - (1 << 64) if v >= (1 << 63) else v for v in packed]          # signed longs
    forced_root = Tag(10, {'data': Tag(10, {'Forced': Tag(12, packed)}),
                           'DataVersion': Tag(3, data['DataVersion'].v)})
    with open(os.path.join(folder, 'data', 'chunks.dat'), 'wb') as f:
        f.write(gzip.compress(serialize('', forced_root)))
    side = json.load(open(template_sidecar))
    side['worldType'] = 1; side['flatLayers'] = ''; side['flatPreset'] = ''
    with open(os.path.join(folder, 'data', 'obeycraft.json'), 'w') as f:
        json.dump(side, f, indent=2)
    return len(per_chunk)
