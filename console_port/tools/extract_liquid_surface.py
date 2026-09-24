#!/usr/bin/env python3
"""Extract original liquid surface heights and the liquid packed-light override."""
from pathlib import Path
import sys
r=Path(__file__).resolve().parents[1]
liquid=(r/'original/reference-only/LiquidTile.cpp').read_text()
renderer=(r/'original/reference-only/TileRenderer.cpp').read_text()
def body(s,signature):
 a=s.index('{',s.index(signature))+1;b=a;depth=1
 while depth:
  if s[b]=='{':depth+=1
  if s[b]=='}':depth-=1
  b+=1
 return s[a:b-1]
depth=body(liquid,'float LiquidTile::getHeight(')
corner=body(renderer,'float TileRenderer::getWaterHeight(')
corner=corner.replace('level->getMaterial( xx, yy + 1, zz ) == m','access.sameLiquid(xx,yy+1,zz)')
corner=corner.replace('Material*\ttm = level->getMaterial( xx, yy, zz );','bool same = access.sameLiquid(xx,yy,zz);')
corner=corner.replace('tm == m','same').replace('!tm->isSolid()','!access.solidMaterial(xx,yy,zz)')
corner=corner.replace('level->getData( xx, yy, zz )','access.data(xx,yy,zz)').replace('LiquidTile::getHeight','consoleLiquidDepth')
light=body(liquid,'int LiquidTile::getLightColor(').replace('level->getLightColor(', 'sampleConsoleLight(access,')
if '--reference' not in sys.argv:
 depth='if(d<0 || d>15)throw std::out_of_range("Invalid liquid data nibble");\n'+depth
 corner='''for(int coordinate:{x,y,z})if(coordinate<=INT_MIN || coordinate>=INT_MAX)throw std::out_of_range("Liquid corner coordinate overflow");
'''+corner.replace('return 1 - h / count;', 'if(count==0)throw std::invalid_argument("Liquid corner has no liquid or open samples");\n\treturn 1 - h / count;')
 light='if(y>=INT_MAX-1)throw std::out_of_range("Liquid light sample height overflow");\n'+light
out='#include "LiquidSurface.h"\n#include <climits>\n#include <stdexcept>\nnamespace console {\n'
out+='float consoleLiquidDepth(int d) {\n'+depth+'}\n'
out+='float consoleLiquidCorner(const LiquidSurfaceAccess& access,int x,int y,int z) {\n'+corner+'}\n'
out+='int sampleConsoleLiquidLight(const RenderLightAccess& access,int x,int y,int z,int tileId) {\n'+light+'}\n}\n'
d=Path(sys.argv[1]);d.parent.mkdir(parents=True,exist_ok=True);d.write_text(out)
