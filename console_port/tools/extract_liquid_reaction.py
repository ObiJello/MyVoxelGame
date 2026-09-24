#!/usr/bin/env python3
"""Extract LiquidTile::updateLiquid, exposing block changes and fizz as a result."""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[1]
s=(root/'original/reference-only/LiquidTile.cpp').read_text()
a=s.index('{',s.index('void LiquidTile::updateLiquid('))+1;b=a;depth=1
while depth:
 if s[b]=='{':depth+=1
 if s[b]=='}':depth-=1
 b+=1
s=s[a:b-1].replace('level->getTile(x, y, z) != id','!access.lava(x,y,z)').replace('return;','return result;').replace('material == Material::lava','access.lava(x,y,z)')
import re
s=re.sub(r'level->getMaterial\((.*?)\) == Material::water',r'access.water(\1)',s)
s=s.replace('level->getData(x, y, z)','access.data(x,y,z)').replace('level->setTile(x, y, z, Tile::obsidian_Id);','result.replacement=49;').replace('level->setTile(x, y, z, Tile::stoneBrick_Id);','result.replacement=4;').replace('fizz(level, x, y, z);','result.fizz=true;')
s=s.replace('int data = access.data(x,y,z);','int data = access.data(x,y,z);\n            if(data<0 || data>15)throw std::out_of_range("Invalid liquid reaction metadata");')
out='''// Generated from original LiquidTile::updateLiquid. Effects are returned to the host.
#include "LiquidReaction.h"
#include <climits>
#include <stdexcept>
namespace console {
LiquidReaction consoleLiquidReaction(const LiquidReactionAccess& access,int x,int y,int z){
    if(x<=INT_MIN || x>=INT_MAX || y>=INT_MAX || z<=INT_MIN || z>=INT_MAX)
        throw std::out_of_range("Liquid reaction coordinate overflow");
    LiquidReaction result;
'''+s+'\nreturn result;\n}\n}\n'
Path(sys.argv[1]).write_text(out)
