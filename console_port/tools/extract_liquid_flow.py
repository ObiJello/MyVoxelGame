#!/usr/bin/env python3
"""Extract original flow vectors, slope, fluid UV rotation and stitched atlas math."""
from pathlib import Path
import re,sys
r=Path(__file__).resolve().parents[1];ref='--reference' in sys.argv
liquid=(r/'original/reference-only/LiquidTile.cpp').read_text()
renderer=(r/'original/reference-only/TileRenderer.cpp').read_text()
stitched=(r/'original/reference-only/StitchedTexture.cpp').read_text()
atlas=(r/'original/reference-only/PreStitchedTextureMap.cpp').read_text()
def body(s,signature):
 a=s.index('{',s.index(signature))+1;b=a;depth=1
 while depth:
  if s[b]=='{':depth+=1
  if s[b]=='}':depth-=1
  b+=1
 return s[a:b-1]
depth=body(liquid,'int LiquidTile::getRenderedDepth(').replace('level->getMaterial(x, y, z) != material','!access.sameLiquid(x,y,z)').replace('level->getData(x, y, z)','access.data(x,y,z)')
if not ref:depth=depth.replace('if (d >= 8)', 'if(d<0 || d>15)throw std::out_of_range("Invalid liquid flow metadata");\n    if (d >= 8)')
face=body(liquid,'bool LiquidTile::isSolidFace(')
face=face.replace('Material *m = level->getMaterial(x, y, z);','').replace('m == this->material','access.sameLiquid(x,y,z)').replace('face == Facing::UP','face == 1').replace('m == Material::ice','access.ice(x,y,z)').replace('Tile::isSolidFace(level, x, y, z, face)','access.solidMaterial(x,y,z)')
flow=body(liquid,'Vec3 *LiquidTile::getFlow(').replace('Vec3','::Vec3')
flow=flow.replace('getRenderedDepth(level,','renderedDepth(access,').replace('level->getMaterial(xt, yt, zt)->blocksMotion()','access.blocksMotion(xt,yt,zt)').replace('level->getData(x, y, z)','access.data(x,y,z)').replace('isSolidFace(level,','solidFace(access,').replace('return flow;', 'return {flow->x,flow->y,flow->z};')
if not ref:flow='''for(int coordinate:{x,y,z})if(coordinate<=INT_MIN || coordinate>=INT_MAX)throw std::out_of_range("Liquid flow coordinate overflow");
    if(!access.sameLiquid(x,y,z))throw std::invalid_argument("Liquid flow requires a liquid source cell");
'''+flow
slope=body(liquid,'double LiquidTile::getSlopeAngle(');slope=slope[slope.index('    if (flow->x'):].replace('flow->','flow.')
out='#include "LiquidFlow.h"\n#include "Vec3.h"\n#include "Mth.h"\n#include <climits>\n#include <cmath>\n#include <stdexcept>\nnamespace console {\n'
out+='static int renderedDepth(const LiquidFlowAccess& access,int x,int y,int z){'+depth+'}\n'
out+='static bool solidFace(const LiquidFlowAccess& access,int x,int y,int z,int face){'+face+'}\n'
out+='LiquidFlow consoleLiquidFlow(const LiquidFlowAccess& access,int x,int y,int z){'+flow+'}\n'
out+='double consoleLiquidSlope(const LiquidFlowAccess& access,int x,int y,int z){auto flow=consoleLiquidFlow(access,x,y,z);\n'+slope+'}\n'
out+='''namespace {
struct LiquidIcon {
 float u0,v0,u1,v1;
 float getU0(bool adjust=false)const;float getU1(bool adjust=false)const;
 float getV0(bool adjust=false)const;float getV1(bool adjust=false)const;
 float getU(double offset,bool adjust=false)const;float getV(double offset,bool adjust=false)const;
};
'''
out+=re.search(r'static const float UVAdjust = .*?;',stitched).group()+'\n'
for method in ['getU0','getU1','getV0','getV1','getU','getV']:
 signature='double offset,bool adjust' if method in ['getU','getV'] else 'bool adjust'
 b=body(stitched,'float StitchedTexture::'+method+'(').replace('SharedConstants::WORLD_RESOLUTION','16')
 out+='float LiquidIcon::'+method+'('+signature+')const{'+b+'}\n'
for name in ['water','water_flow','lava','lava_flow']:
 match=re.search(r'new SimpleIcon\(L"'+name+r'",(.*?)\)\)\);',atlas)
 rect=match.group(1).replace('slotSize','(1.0f/16.0f)')
 out+='constexpr LiquidIcon '+name+'Icon{'+rect+'};\n'
out+='}\nstd::array<LiquidUV,4> consoleLiquidTopUV(float angle,bool lava){\n'
if not ref:out+='if(!std::isfinite(angle) || (angle!=-1000 && std::abs(angle)>7))throw std::invalid_argument("Invalid liquid slope angle");\n'
out+='auto* tex=angle < -999?(lava?&lavaIcon:&waterIcon):(lava?&lava_flowIcon:&water_flowIcon);\n'
start=renderer.index('float u00, u01, u10, u11;',renderer.index('bool TileRenderer::tesselateWaterInWorld'))
end=renderer.index('\n\t\tfloat\tbr;',start)
uv=renderer[start:end].replace('SharedConstants::WORLD_RESOLUTION','16')
out+=uv+'\nreturn {{{u00,v00},{u01,v01},{u10,v10},{u11,v11}}};\n}\n'
out+='LiquidUV consoleLiquidSideUV(float height,bool right,bool lava){\n'
if not ref:out+='if(!std::isfinite(height) || height < -.001f || height>1)throw std::invalid_argument("Invalid liquid side height");\n'
out+='''const auto& tex=lava?lava_flowIcon:water_flowIcon;
return {tex.getU(right?8:0,true),height==0?tex.getV(8,true):tex.getV((1-height)*8)};
}
}
'''
d=Path(sys.argv[1]);d.parent.mkdir(parents=True,exist_ok=True);d.write_text(out)
