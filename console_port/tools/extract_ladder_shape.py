#!/usr/bin/env python3
from pathlib import Path
import sys
r=Path(__file__).resolve().parents[1]
def body(file,signature):
 s=(r/'original/reference-only'/file).read_text();a=s.index('{',s.index(signature));b=a+1;d=1
 while d:
  d+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
shape=body('LadderTile.cpp','void LadderTile::setShape(int data)')
render=body('TileRenderer.cpp','bool TileRenderer::tesselateLadderInWorld(')
a=render.index('float\t\to =');render=render[a:render.index('return true;')]
mob=(r/'original/reference-only/Mob.cpp').read_text();a=mob.index('float max = 0.15f;',mob.index('if (onLadder())'));b=mob.index('\n\t\t}',a)
clamp=mob[a:b].replace('this->fallDistance = 0;','').replace('bool playerSneaking = isSneaking() && dynamic_pointer_cast<Player>(shared_from_this()) != NULL;', 'bool playerSneaking = sneaking;')
out='''// Generated from LadderTile, TileRenderer and Mob ladder movement.
#include "LadderShape.h"
#include <stdexcept>
namespace console {
BlockShape consoleLadderShape(int data){
 BlockShape result;result.count=1;result.boxes[0]={0,0,0,1,1,1};
 auto setShape=[&](double x0,double y0,double z0,double x1,double y1,double z1){result.boxes[0]={x0,y0,z0,x1,y1,z1};};
'''+shape+'''
 return result;
}
std::array<LadderVertex,4> consoleLadderQuad(int face){
 if(face<2 || face>5)throw std::invalid_argument("Invalid ladder facing");
 struct Quad {std::array<LadderVertex,4> vertices{};int count=0;
 void vertexUV(float x,float y,float z,float u,float v){vertices.at(count++)={x,y,z,u,v};}}quad;
 auto* t=&quad;constexpr int x=0,y=0,z=0;constexpr float u0=0,v0=0,u1=1,v1=1;
'''+render+'''
 return quad.vertices;
}
Vec3 consoleLadderVelocity(Vec3 perSecond,bool sneaking){
 // Original Mob velocities are blocks per 20 Hz tick; native movement uses seconds.
 double xd=perSecond.x/20,yd=perSecond.y/20,zd=perSecond.z/20;
'''+clamp+'''
 return {xd*20,yd*20,zd*20};
}
}
'''
Path(sys.argv[1]).write_text(out)
