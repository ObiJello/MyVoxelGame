#!/usr/bin/env python3
"""Adapt original GrassTile 3x3 channel average and texture-pack tint constants."""
from pathlib import Path
import sys,xml.etree.ElementTree as ET
r=Path(__file__).resolve().parents[1];s=(r/'original/reference-only/GrassTile.cpp').read_text()
a=s.index('int totalRed',s.index('int GrassTile::getColor(LevelSource *level, int x, int y, int z, int data)'));b=s.index('\n}',a)
body=s[a:b].replace('level->getBiome(x + ox, z + oz)->getGrassColor()','colours[static_cast<int>(kind)][biomes[(oz+1)*3+ox+1]]')
c={x.attrib['name']:x.attrib['value'] for x in ET.parse(r/'assets/colours.xml').getroot().findall('colour')}
names=['Ocean','Plains','Desert','ExtremeHills','Forest','Taiga','Swampland','River','Hell','Sky','FrozenOcean','FrozenRiver','IcePlains','IceMountains','MushroomIsland','MushroomIslandShore','Beach','DesertHills','ForestHills','TaigaHills','ExtremeHillsEdge','Jungle','JungleHills']
out='''// Generated from GrassTile::getColor; LeafTile/LiquidTile use the same average.
#include "BiomeTint.h"
#include <stdexcept>
namespace console {
int consoleBiomeTint(TintKind kind,const std::array<int,9>& biomes,int leafData){
 if(static_cast<int>(kind)<0 || static_cast<int>(kind)>2 || leafData<0 || leafData>15)throw std::invalid_argument("Invalid biome tint input");
 for(int id:biomes)if(id<0 || id>=23)throw std::invalid_argument("Invalid tint biome");
'''
out+=' if(kind==TintKind::Foliage && (leafData&3)==1)return 0x'+c['Foliage_Evergreen']+';\n'
out+=' if(kind==TintKind::Foliage && (leafData&3)==2)return 0x'+c['Foliage_Birch']+';\n'
out+=' static constexpr int colours[3][23]={\n'
for kind in ['Grass','Foliage','Water']:out+='{'+','.join('0x'+c[kind+'_'+name] for name in names)+'},\n'
out+=' };\n'+body+'\n}\n}\n';Path(sys.argv[1]).write_text(out)
