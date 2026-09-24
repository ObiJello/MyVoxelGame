#!/usr/bin/env python3
"""Extract original full-cube face UV arithmetic and tree rotation selection."""
from pathlib import Path
import sys,re
r=Path(__file__).resolve().parents[1];s=(r/'original/reference-only/TileRenderer.cpp').read_text()
def body(signature):
 a=s.index('{',s.index(signature));b=a+1;depth=1
 while depth:
  depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a+1:b-1]
names=['renderFaceUp','renderFaceDown','renderWest','renderEast','renderNorth','renderSouth']
out='''// Generated from TileRenderer full-cube face routines and tesselateTreeInWorld.
#include "BlockFaceUV.h"
#include <stdexcept>
namespace console { namespace {
struct SharedConstants {static constexpr float WORLD_RESOLUTION=16;};
struct TreeTile {enum {MASK_FACING=12,FACING_X=4,FACING_Z=8};};
struct Icon {
 float getU(float x,bool)const{return x/16;}
 float getV(float x,bool)const{return x/16;}
 float getU0(bool)const{return 0;} float getU1(bool)const{return 1;}
 float getV0(bool)const{return 0;} float getV1(bool)const{return 1;}
};
struct Point {float x,y,z,u,v;};
struct Quad {std::array<Point,4> points{};int count=0;
 void vertexUV(float x,float y,float z,float u,float v){points.at(count++)={x,y,z,u,v};}
};
struct FaceBuilder {
 enum {FLIP_CW=1,FLIP_CCW=2,FLIP_180=3};
 float tileShapeX0=0,tileShapeY0=0,tileShapeZ0=0,tileShapeX1=1,tileShapeY1=1,tileShapeZ1=1;
 bool xFlipTexture=false;
 int eastFlip=0,northFlip=0,southFlip=0,westFlip=0,upFlip=0,downFlip=0;
 void tree(int data){
'''
tree=body('bool TileRenderer::tesselateTreeInWorld(');a=tree.index('int facing');b=tree.index('bool result');out+=tree[a:b]+'}\n'
for name in names:
 raw=body('void TileRenderer::'+name+'(');prefix=raw[:raw.index('if ( applyAmbienceOcclusion )')];prefix=prefix.replace('Tesselator* t = Tesselator::getInstance();','Quad quad; Quad* t=&quad; Icon icon;Icon* tex=&icon;').replace('if (hasFixedTexture()) tex = fixedTexture;','')
 # Use the final plain vertex branch, excluding the platform-specific AO branch.
 tail=raw[raw.rindex('\n\telse'):];tail=tail[tail.index('{')+1:tail.rindex('}')]
 assert tail.count('t->vertexUV')==4
 out+='Quad '+name+'(){ constexpr double x=0,y=0,z=0;\n'+prefix+tail+'\nreturn quad;\n}\n'
out+='''};
using Faces=std::array<std::array<BlockUV,4>,6>;
Faces makeFaces(int data,BlockBox box={0,0,0,1,1,1}){FaceBuilder builder;builder.tree(data);
 builder.tileShapeX0=box.x0;builder.tileShapeY0=box.y0;builder.tileShapeZ0=box.z0;
 builder.tileShapeX1=box.x1;builder.tileShapeY1=box.y1;builder.tileShapeZ1=box.z1;
 const Quad quads[]={'''+','.join('builder.'+n+'()' for n in names)+'''};
 // Match original vertices by position to the native mesh corner ordering.
 constexpr int corners[6][4][3]={
 {{0,1,0},{0,1,1},{1,1,1},{1,1,0}},{{0,0,1},{0,0,0},{1,0,0},{1,0,1}},
 {{0,1,1},{0,1,0},{0,0,0},{0,0,1}},{{1,1,0},{1,1,1},{1,0,1},{1,0,0}},
 {{0,1,0},{1,1,0},{1,0,0},{0,0,0}},{{1,1,1},{0,1,1},{0,0,1},{1,0,1}}};
 Faces result{};
 for(int f=0;f<6;++f)for(int k=0;k<4;++k){bool found=false;
  for(const auto& p:quads[f].points)if(p.x==float(corners[f][k][0]?box.x1:box.x0) && p.y==float(corners[f][k][1]?box.y1:box.y0) && p.z==float(corners[f][k][2]?box.z1:box.z0)){
   result[f][k]={p.u,p.v};found=true;break;}
  if(!found)throw std::logic_error("Original face vertex missing");
 }
 return result;
}
}
std::array<BlockUV,4> consoleBoxFaceUV(Block block,int face,int data,BlockBox box){
 if(face<0 || face>=6 || data<0 || data>15)throw std::invalid_argument("Invalid block face UV input");
 return makeFaces(block==Log?data:0,box)[face];
}
const std::array<BlockUV,4>& consoleBlockFaceUV(Block block,int face,int data){
 if(face<0 || face>=6 || data<0 || data>15)throw std::invalid_argument("Invalid block face UV input");
 static const std::array<Faces,4> faces={makeFaces(0),makeFaces(4),makeFaces(8),makeFaces(12)};
 return faces[block==Log?data/4:0][face];
}
}
'''
Path(sys.argv[1]).write_text(out)
