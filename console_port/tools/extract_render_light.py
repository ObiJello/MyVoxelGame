#!/usr/bin/env python3
"""Transfer Level's propagated render light lookup and packed light coordinates."""
from pathlib import Path
import sys
r=Path(__file__).resolve().parents[1]
s=(r/'original/reference-only/Level.cpp').read_text()
a=s.index('int Level::getBrightnessPropagate(');b=s.index('\nint Level::getBrightness(',a)
body=s[a:b]
body=body.replace('int Level::getBrightnessPropagate(LightLayer::variety layer, int x, int y, int z, int tileId)', 'static int propagated(const RenderLightAccess& access,LightLayer::variety layer,int x,int y,int z,int tileId)')
for old,new in [('dimension->hasCeiling','access.hasCeiling()'),('maxBuildHeight','256'),('MAX_LEVEL_SIZE','30000000'),('hasChunk(','access.hasChunk('),('getTile(','access.tile('),('Tile::propagate[id]','access.propagates(id)'),('getBrightness(layer,','access.brightness(layer,')]:body=body.replace(old,new)
body=body.replace('LevelChunk *c = getChunk(xc, zc);\n    return c->access.brightness(layer, x & 15, y, z & 15);','return access.storedLight(layer,x,y,z);')
a=s.index('int Level::getLightColor(');b=s.index('\nfloat Level::getBrightness(',a)
pack=s[a:b].replace('int Level::getLightColor(int x, int y, int z, int emitt, int tileId/*=-1*/)','int sampleConsoleLight(const RenderLightAccess& access,int x,int y,int z,int emitt,int tileId)')
pack=pack.replace('getBrightnessPropagate(', 'propagated(access,')
if '--reference' not in sys.argv:
    pack=pack.replace('{','''{
    for(int coordinate:{x,y,z})if(coordinate <= INT_MIN || coordinate >= INT_MAX)throw std::out_of_range("Render light coordinate overflow");
    if(emitt<0 || emitt>15 || tileId < -1 || tileId > 254)throw std::out_of_range("Invalid render light tile/emission");''',1)
out='#include "RenderLightAccess.h"\n#include <climits>\n#include <stdexcept>\nnamespace console {\n'+body+pack+'\n}\n'
d=Path(sys.argv[1]);d.parent.mkdir(parents=True,exist_ok=True);d.write_text(out)
