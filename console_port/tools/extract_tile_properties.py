#!/usr/bin/env python3
"""Per-tile properties from the original Tile registry: ported/TileProperties.cpp.

For every tile Tile::staticCtor registers this follows its constructor chain
(each class's constructor and the base constructor it calls, with the
registration's arguments substituted) and the registration's setter chain:

* class and material (the material as tools/extract_survival_tiles.py resolved
  it, read back from ported/TileSurvival.cpp);
* solid render: the isSolidRender argument that reaches Tile(id, material,
  isSolidRender) — a literal, a constructor parameter, or the constructing
  class's own isSolidRender() (constructors call it without virtual dispatch);
  HalfSlabTile's full-size assignment to solid[] is applied;
* light block: 255 for solid-render tiles, else 0, then setLightBlock calls;
* light emission: (int)(15 * f) for setLightEmission(f);
* ticking: setTicking(true) in a constructor (conditions on a boolean
  constructor parameter are evaluated) or the setter chain;
* cube shaped: the nearest isCubeShaped() override (HalfSlabTile: fullSize);
* destroy time and explosion resistance: setDestroyTime (which raises the
  resistance to five times the time), setExplodeable (three times its
  argument) and setIndestructible (-1), in constructor and chain order;
  StairTile and WallTile take their base tile's (the constructor's
  setDestroyTime(base->destroySpeed) / setExplodeable(base->explosionResistance / 3)).

LeafTile passes isSolidRender(), which returns !allowSame before the
constructor has set allowSame: that read is uninitialised in the source. The
table records it as not solid (the value the port has always used) and marks it.

Run with --check to fail when ported/TileProperties.cpp is out of date.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'source_full/Minecraft.World'
OUT = ROOT / 'ported/TileProperties.cpp'


# The console sources come from a case-insensitive filesystem.
FILES = {p.name.lower(): p for p in SRC.iterdir()}


def read(name):
    path = FILES.get(name.lower())
    if path is None:
        return ''
    text = path.read_text(encoding='utf-8-sig', errors='replace')
    return re.sub(r'//[^\n]*|/\*(?!\s*=).*?\*/', '', text, flags=re.S)


def split_args(text):
    args, depth, cur = [], 0, ''
    for ch in text:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        args.append(cur.strip())
    return args


def balanced(text, start):
    """Index just past the ')' matching the '(' at text[start]."""
    depth = 0
    for i in range(start, len(text)):
        depth += (text[i] == '(') - (text[i] == ')')
        if depth == 0:
            return i + 1
    raise ValueError('unbalanced')


tile_h = read('Tile.h')
IDS = {m[1]: int(m[2]) for m in re.finditer(r'static const int (\w+)_Id\s*=\s*(\d+)', tile_h)}

CTORS = {}


def constructors(cls):
    """[(params [(name, default)], base, base_args, body)] for a class."""
    if cls in CTORS:
        return CTORS[cls]
    src = read(cls + '.cpp')
    result = []
    for m in re.finditer(re.escape(cls) + r'::' + re.escape(cls) + r'\s*\(', src):
        close = balanced(src, m.end() - 1)
        params = []
        for p in split_args(src[m.end():close - 1]):
            default = re.search(r'/\*\s*=\s*(\w+)\s*\*/|=\s*(\w+)\s*$', p)
            p = re.sub(r'/\*.*?\*/|=\s*\w+\s*$', '', p).strip()
            name = re.findall(r'\w+', p)[-1] if p else ''
            params.append((name, (default[1] or default[2]) if default else None))
        rest = src[close:]
        init = re.match(r'\s*:\s*(\w+)\s*\(', rest)
        base, base_args = None, []
        if init:
            base = init[1]
            open_at = close + init.end() - 1
            end = balanced(src, open_at)
            base_args = split_args(src[open_at + 1:end - 1])
            rest = src[end:]
        body_start = rest.index('{')
        depth, i = 0, body_start
        while True:
            depth += (rest[i] == '{') - (rest[i] == '}')
            i += 1
            if depth == 0:
                break
        result.append((params, base, base_args, rest[body_start:i]))
    # Header defaults (e.g. EntityTile(int id, Material *m, bool isSolidRender = true)).
    header = read(cls + '.h')
    for h in re.finditer(re.escape(cls) + r'\s*\(([^)]*)\)\s*;', header):
        hparams = split_args(h[1])
        for ctor in result:
            if len(ctor[0]) == len(hparams):
                for k, hp in enumerate(hparams):
                    d = re.search(r'=\s*(\w+)\s*$', hp)
                    if d and ctor[0][k][1] is None:
                        ctor[0][k] = (ctor[0][k][0], d[1])
    CTORS[cls] = result
    return result


def method_body(cls, method):
    """The body of cls::method (in the .cpp or inline in the .h), or None."""
    src = read(cls + '.cpp')
    m = re.search(r'\bbool\s+' + re.escape(cls) + r'::' + method + r'\s*\([^)]*\)\s*\{(.*?)\n\}', src, re.S)
    if m:
        return m[1]
    m = re.search(r'\bbool\s+' + method + r'\s*\([^)]*\)\s*\{([^}]*)\}', read(cls + '.h'))
    return m[1] if m else None


def bases_of(cls):
    chain = [cls]
    while True:
        ctors = constructors(chain[-1])
        base = next((c[1] for c in ctors if c[1]), None)
        if not base or base == 'Tile' or base in chain:
            return chain
        chain.append(base)


def override(cls, method):
    for c in bases_of(cls):
        body = method_body(c, method)
        if body is not None:
            return c, body
    return None, None


def truth(expr, env):
    expr = expr.strip()
    if expr in ('true', 'false'):
        return expr == 'true'
    if expr in env:
        return env[expr]
    raise ValueError(f'cannot evaluate {expr!r}')


def pick(ctors, count):
    fits = [c for c in ctors if sum(1 for p in c[0] if p[1] is None) <= count <= len(c[0])]
    if not fits:
        raise ValueError('no constructor for %d arguments' % count)
    return fits[0]


def evaluate(cls, args, row):
    """Walk the constructor chain of cls called with args (strings)."""
    ctor = pick(constructors(cls), len(args))
    params, base, base_args, body = ctor
    env = {}
    for k, (name, default) in enumerate(params):
        value = args[k] if k < len(args) else default
        env[name] = value
    bools = {}
    for name, value in env.items():
        if value in ('true', 'false'):
            bools[name] = value == 'true'
    # The base constructor first.
    if base == 'Tile':
        solid_arg = base_args[2] if len(base_args) > 2 else 'true'
        solid_arg = solid_arg.strip()
        if solid_arg.replace(' ', '') == 'isSolidRender()':
            owner, fn = override(cls, 'isSolidRender')
            if owner == 'LeafTile':
                row['solid'], row['note'] = False, 'isSolidRender() reads allowSame before it is set'
            else:
                ret = re.search(r'return\s+(\w+)\s*;', fn or 'return true;')[1]
                row['solid'] = truth(ret, bools)
        else:
            row['solid'] = truth(env.get(solid_arg, solid_arg) if solid_arg in env else solid_arg, bools)
        row['lightBlock'] = 255 if row['solid'] else 0
    elif base:
        resolved = []
        for a in base_args:
            a = a.strip()
            if a.replace(' ', '') == 'isSolidRender()':
                owner, fn = override(cls, 'isSolidRender')
                if owner == 'LeafTile':
                    row['note'] = 'isSolidRender() reads allowSame before it is set'
                    resolved.append('false')
                else:
                    resolved.append('true' if truth(re.search(r'return\s+(\w+)\s*;', fn)[1], bools) else 'false')
            elif a in env and env[a] is not None:
                resolved.append(env[a])
            else:
                resolved.append(a)
        evaluate(base, resolved, row)
    # Then this constructor's body, in order, with its class's _init() helper.
    if re.search(r'\b_init\s*\(\s*\)', body):
        helper = re.search(r'void\s+' + re.escape(cls) + r'::_init\s*\(\s*\)\s*\{(.*?)\n\}', read(cls + '.cpp'), re.S)
        if helper:
            body = re.sub(r'\b_init\s*\(\s*\)\s*;', helper[1], body)
    for stmt in re.finditer(r'(?:if\s*\(\s*(!?)\s*(\w+)\s*\)\s*\{?[^;{}]*?)?\b(setTicking|setLightBlock|setLightEmission|setDestroyTime|setExplodeable|setIndestructible)\s*\(([^)]*)\)|solid\[id\]\s*=\s*(\w+)', body):
        if stmt[5]:
            cond = re.search(r'if\s*\(\s*(\w+)\s*\)\s*\{[^}]*solid\[id\]', body)
            if not cond or truth(cond[1], bools):
                row['solid'] = truth(stmt[5], bools)
            continue
        if stmt[2]:
            ok = truth(stmt[2], bools)
            if stmt[1]:
                ok = not ok
            if not ok:
                continue
        apply(row, stmt[3], stmt[4])


def apply(row, setter, arg):
    arg = arg.strip()
    if setter == 'setTicking':
        row['ticking'] = arg == 'true'
    elif setter == 'setLightBlock':
        row['lightBlock'] = int(arg)
    elif setter == 'setLightEmission':
        m = re.fullmatch(r'([\d.]+)f?\s*(?:/\s*([\d.]+)f?)?', arg)
        value = float(m[1]) / (float(m[2]) if m[2] else 1.0)
        row['emission'] = int(15 * value)
    elif setter in ('setDestroyTime', 'setIndestructible'):
        if setter == 'setIndestructible' or arg == 'INDESTRUCTIBLE_DESTROY_TIME':
            value = -1.0
        elif re.fullmatch(r'-?[\d.]+f?', arg):
            value = float(arg.rstrip('f'))
        else:
            row['copyBase'] = True  # StairTile/WallTile: base->destroySpeed
            return
        row['destroy'] = value
        row['resist'] = max(row['resist'], value * 5)
    elif setter == 'setExplodeable':
        if re.fullmatch(r'-?[\d.]+f?', arg):
            row['resist'] = float(arg.rstrip('f')) * 3
        else:
            row['copyBase'] = True


def materials():
    text = (ROOT / 'ported/TileSurvival.cpp').read_text()
    return {int(m[1]): m[2] for m in re.finditer(r'\{(\d+),[^,]+,SurvivalMaterial::(\w+),', text)}


def main():
    source = read('Tile.cpp')
    start = source.index('void Tile::staticCtor()')
    body = source[start:source.index('\nvoid Tile::', start + 10)]
    material = materials()
    rows = {}
    for statement in body.split(';'):
        reg = re.search(r'Tile::(\w+)\s*=[^;]*?new\s+(\w+)\s*\(', statement)
        if not reg:
            continue
        name, cls = reg[1], reg[2]
        close = balanced(statement, reg.end() - 1)
        args = split_args(statement[reg.end():close - 1])
        first = args[0] if args else ''
        if re.fullmatch(r'\d+', first):
            tile_id = int(first)
        elif re.fullmatch(r'Tile::\w+_Id', first):
            tile_id = IDS[first[6:-3]]
        elif name in IDS:
            tile_id = IDS[name]
        else:
            raise SystemExit(f'Unresolved tile id for {name}')
        row = dict(name=name, cls=cls, solid=True, lightBlock=255, emission=0, ticking=False, note='',
                   destroy=0.0, resist=0.0)
        try:
            evaluate(cls, args, row)
        except (ValueError, KeyError, TypeError, AttributeError) as error:
            raise SystemExit(f'{name} ({cls}): {error}')
        for setter, arg in re.findall(r'->\s*(setTicking|setLightBlock|setLightEmission|setDestroyTime|setExplodeable|setIndestructible)\s*\(([^)]*)\)', statement[close:]):
            apply(row, setter, arg)
        if row.pop('copyBase', False):
            base = re.fullmatch(r'Tile::(\w+)', args[1].strip()) if len(args) > 1 else None
            if not base:
                raise SystemExit(f'{name}: no base tile for its destroy time')
            row['base'] = base[1]
        owner, fn = override(cls, 'isCubeShaped')
        if fn is None:
            row['cube'] = True
        else:
            ret = re.search(r'return\s+(\w+)\s*;', fn)[1]
            row['cube'] = {'true': True, 'false': False}.get(ret, row['solid'] if ret == 'fullSize' else None)
            if row['cube'] is None:
                raise SystemExit(f'{name}: isCubeShaped returns {ret}')
        if tile_id not in material:
            raise SystemExit(f'{name}: no material in ported/TileSurvival.cpp')
        row['material'] = material[tile_id]
        rows[tile_id] = row

    by_name = {r['name']: r for r in rows.values()}
    for r in rows.values():
        if 'base' in r:
            base = by_name[r.pop('base')]
            r['destroy'], r['resist'] = base['destroy'], base['resist']
    num = lambda v: repr(float(v)) + 'f'
    out = ['// Generated by tools/extract_tile_properties.py from original Tile::staticCtor',
           '// and the tile constructors. Do not edit by hand.',
           '#include "TileProperties.h"', 'namespace console {', 'namespace {',
           'constexpr TileProperties kTiles[]{']
    for tile_id, r in sorted(rows.items()):
        flag = lambda b: 'true' if b else 'false'
        note = f' {r["note"]}' if r['note'] else ''
        out.append(f' {{{tile_id},"{r["cls"]}",SurvivalMaterial::{r["material"]},{flag(r["solid"])},'
                   f'{r["lightBlock"]},{r["emission"]},{flag(r["ticking"])},{flag(r["cube"])},'
                   f'{num(r["destroy"])},{num(r["resist"])}}}, // {r["name"]}{note}')
    out += ['};', '}',
            'const TileProperties* consoleTileProperties(int id){',
            ' static const TileProperties* const* table=[]{',
            '  static const TileProperties* byId[256]{};',
            '  for(const auto& tile:kTiles)byId[tile.id]=&tile;',
            '  return byId;', ' }();',
            ' return id>=0 && id<256?table[id]:nullptr;', '}', '}', '']
    text = '\n'.join(out)
    if '--check' in sys.argv:
        if not OUT.exists() or OUT.read_text() != text:
            sys.exit('ported/TileProperties.cpp is out of date: run tools/extract_tile_properties.py')
        print('ported/TileProperties.cpp is current')
        return
    OUT.write_text(text)
    print(f'{len(rows)} tiles')


if __name__ == '__main__':
    main()
