#!/usr/bin/env python3
"""Keep original Level/Tile ray arithmetic, replacing the world/shape boundary."""
from pathlib import Path
import re,sys
r=Path(__file__).resolve().parents[1];reference='--reference' in sys.argv
def method(file,signature):
    s=(r/'original/reference-only'/file).read_text();a=s.index(signature);b=s.index('{',a)+1;depth=1
    while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
    return s[a:b]
level=method('Level.cpp','HitResult *Level::clip(Vec3 *a, Vec3 *b, bool liquid, bool solidOnly)')
level=level.replace('Level::','BlockRaycaster::').replace('Double::isNaN','std::isnan')
level=level.replace('Tile *tile = Tile::tiles[t];','RaycastTile *tile = tileFor(t);')
if not reference:
    at=level.index('{')+1
    level=level[:at]+'''
    if(!a || !b)return nullptr;
    for(double coordinate:{a->x,a->y,a->z,b->x,b->y,b->z})
        if(!std::isfinite(coordinate) || coordinate<double(INT32_MIN)+2 || coordinate>double(INT32_MAX)-2)return nullptr;
'''+level[at:]
    level=level.replace('RaycastTile *tile = tileFor(t);','RaycastTile *tile = tileFor(t);\n        if(t>0 && !tile)throw std::logic_error("Unregistered raycast tile");')
tile=method('Tile.cpp','HitResult *Tile::clip(Level *level, int xt, int yt, int zt, Vec3 *a, Vec3 *b)')
tile=tile.replace('Tile::clip(Level *level','BoxRaycastTile::clip(BlockRaycaster *level')
tile=tile.replace('updateShape(level, xt, yt, zt);','AABB* bounds = shape(level, xt, yt, zt);\n    if(!bounds)return nullptr;')
tile=tile.replace('ThreadStorage *tls = (ThreadStorage *)TlsGetValue(Tile::tlsIdxShape);','')
for old,new in [('xx0','x0'),('xx1','x1'),('yy0','y0'),('yy1','y1'),('zz0','z0'),('zz1','z1')]:tile=tile.replace('tls->'+old,'bounds->'+new)
for axis in 'XYZ':tile=tile.replace('contains'+axis+'(', 'bounds->contains'+axis+'(')
out='#include "BlockRaycaster.h"\n#include "Mth.h"\n#include "Facing.h"\n#include <cmath>\n#include <stdexcept>\nnamespace console {\n'
out+=re.sub(r'\bVec3\b','::Vec3',level+'\n'+tile)+'\n}\n'
Path(sys.argv[1]).write_text(out)
