#!/usr/bin/env python3
"""Extract Level sky tint arithmetic and the original texture-pack biome palette."""
from pathlib import Path
import sys,xml.etree.ElementTree as ET
r=Path(__file__).resolve().parents[1];s=(r/'original/reference-only/Level.cpp').read_text()
a=s.index('    ',0) if False else s.index('Vec3 *Level::getSkyColor(')
a=s.index('float r =',a);b=s.index('return Vec3::newTemp(r, g, b);',a)
body=s[a:b].replace('getRainLevel(a)','rain').replace('getThunderLevel(a)','thunder')
colours={x.attrib['name']:x.attrib['value'] for x in ET.parse(r/'assets/colours.xml').getroot().findall('colour')}
names=['Ocean','Plains','Desert','ExtremeHills','Forest','Taiga','Swampland','River','Hell','Sky','FrozenOcean','FrozenRiver','IcePlains','IceMountains','MushroomIsland','MushroomIslandShore','Beach','DesertHills','ForestHills','TaigaHills','ExtremeHillsEdge','Jungle','JungleHills']
out='''// Generated from Level::getSkyColor and the original colours.xml.
#include "SkyColour.h"
#include "ConsoleLightmap.h"
#include "Mth.h"
#include <cmath>
#include <stdexcept>
namespace console {
std::array<float,3> consoleSkyColour(int biome,std::int64_t time,float rain,float thunder,int lightningBoltTime,float a){
    if(biome<0 || biome>=23 || !std::isfinite(rain) || rain<0 || rain>1 || !std::isfinite(thunder) || thunder<0 || thunder>1 || lightningBoltTime<0)
        throw std::invalid_argument("Invalid sky colour input");
    static constexpr int colours[]={'''+','.join('0x'+colours['Sky_'+name] for name in names)+'''};
    int skyColor=colours[biome];
    float td=consoleTimeOfDay(time,a);
    float br=Mth::cos(td * PI * 2) * 2 + 0.5f;
    if(br<0)br=0;
    if(br>1)br=1;
    '''+body+'return {r,g,b};\n}\n}\n'
source=(r/'original/reference-only/Level.cpp').read_text()
a=source.index('{',source.index('Vec3 *Level::getCloudColor('))+1;b=source.index('return Vec3::newTemp(r, g, b);',a)
body=source[a:b].replace('getTimeOfDay(a)','consoleTimeOfDay(time,a)').replace('getRainLevel(a)','rain').replace('getThunderLevel(a)','thunder').replace('Minecraft::GetInstance()->getColourTable()->getColor( eMinecraftColour_In_Cloud_Base_Colour )','0x'+colours['In_Cloud_Base_Colour'])
out=out[:-2]+"std::array<float,3> consoleCloudColour(std::int64_t time,float rain,float thunder,float a){\n"+"if(!std::isfinite(rain) || rain<0 || rain>1 || !std::isfinite(thunder) || thunder<0 || thunder>1)throw std::invalid_argument(\"Invalid cloud colour input\");\n"+body+"return {r,g,b};\n}\n}\n"
Path(sys.argv[1]).write_text(out)
