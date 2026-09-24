#!/usr/bin/env python3
"""Extract DoorTile composite metadata, bounds and texture selection."""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[1]
s=(root/'original/reference-only/DoorTile.cpp').read_text()
def body(signature):
 a=s.index('{',s.index(signature));b=a+1;depth=1
 while depth:
  depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
out='''// Generated from original DoorTile.cpp by tools/extract_door_shape.py.
#include "DoorShape.h"
#include <stdexcept>
namespace console {namespace {
constexpr int UPPER_BIT=8,C_DIR_MASK=3,C_OPEN_MASK=4,C_LOWER_DATA_MASK=7,C_IS_UPPER_MASK=8,C_RIGHT_HINGE_MASK=16;
using LevelSource=const BlockShapeAccess;
struct Facing {enum {UP=1,DOWN=0};};
}
int consoleDoorData(const BlockShapeAccess& access,int x,int y,int z){const auto* level=&access;
'''+body('int DoorTile::getCompositeData(')[1:]+'''
BlockShape consoleDoorShape(const BlockShapeAccess& access,int x,int y,int z){
 int compositeData=consoleDoorData(access,x,y,z);BlockShape result;result.count=1;
 auto setShape=[&](double x0,double y0,double z0,double x1,double y1,double z1){result.boxes[0]={x0,y0,z0,x1,y1,z1};};
'''+body('void DoorTile::setShape(int compositeData)').replace('Tile::setShape','setShape')[1:-1]+'''
 return result;
}
DoorTexture consoleDoorTexture(const BlockShapeAccess& access,int x,int y,int z,int nativeFace){
 if(nativeFace<0 || nativeFace>5)throw std::invalid_argument("Invalid door face");
 constexpr int faces[]={1,0,4,5,2,3};int face=faces[nativeFace];
 const auto* level=&access;constexpr int DOOR_TILE_TEXTURE_COUNT=4;
 const DoorTexture icons[]={{97,false},{81,false},{98,false},{82,false},{97,true},{81,true},{98,true},{82,true}};
 int texBase=access.getTile(x,y,z)==71?2:0;
'''+body('Icon *DoorTile::getTexture(LevelSource').replace('getCompositeData(level, x, y, z)','consoleDoorData(access, x, y, z)')[1:]+'''
}
'''
Path(sys.argv[1]).write_text(out)
