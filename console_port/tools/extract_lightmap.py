#!/usr/bin/env python3
"""Extract original console CPU lightmap, dimension ramp and celestial formulas."""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[1]
ref='--reference' in sys.argv
game=(root/'original/reference-only/GameRenderer.cpp').read_text()
dimension=(root/'original/reference-only/Dimension.cpp').read_text()
level=(root/'original/reference-only/Level.cpp').read_text()
def body(source,signature):
    start=source.index('{',source.index(signature))+1
    end=start;depth=1
    while depth:
        if source[end]=='{':depth+=1
        if source[end]=='}':depth-=1
        end+=1
    return source[start:end-1]
time=body(dimension,'float Dimension::getTimeOfDay(').replace('Level::TICKS_PER_DAY','24000')
sky=body(level,'float Level::getSkyDarken(').replace('getTimeOfDay(a)','consoleTimeOfDay(time,a,dimension)').replace('getRainLevel(a)','rain').replace('getThunderLevel(a)','thunder')
ramp=body(dimension,'void Dimension::updateLightRamp(').replace('float ambientLight = 0.00f;', 'float ambientLight = input.dimension == -1 ? 0.10f : 0.00f;').replace('Level::MAX_BRIGHTNESS','15')
night=body(game,'float GameRenderer::getNightVisionScale(')
night=night.replace('int duration = player->getEffect(MobEffect::nightVision)->getDuration();','').replace('SharedConstants::TICKS_PER_SECOND','20')
night=night.replace('max(', 'std::max(')
flicker=body(game,'void GameRenderer::tickLightTexture(').replace('Math::random()','randomValues[index++]').replace('_updateLightTexture = true;','')
# One expression must not increment the same index through unsequenced operands.
for expression in ['(randomValues[index++] - randomValues[index++]) * randomValues[index++] * randomValues[index++]']:
    flicker=flicker.replace(expression,'(randomValues[0] - randomValues[1]) * randomValues[2] * randomValues[3]',1)
    flicker=flicker.replace(expression,'(randomValues[4] - randomValues[5]) * randomValues[6] * randomValues[7]',1)
start=game.index('float darken = skyDarken1 * 0.95f + 0.05f;')
end=game.index('\n#if ( defined _DURANGO',start)
loop=game[start:end]
for old,new in [('level->dimension->brightnessRamp','brightnessRamp'),('level->lightningBoltTime > 0','input.lightning'),('level->dimension->id','input.dimension'),('player->hasEffect(MobEffect::nightVision)','input.nightVisionTicks >= 0'),('getNightVisionScale(player, a)','consoleNightVisionScale(input.nightVisionTicks, input.partialTick)')]:loop=loop.replace(old,new)
if ref:
    pack='''const std::uint32_t ps3 = std::uint32_t(r) << 24 | std::uint32_t(g) << 16 | std::uint32_t(b) << 8 | std::uint32_t(a);
        pixels[i] = {std::uint8_t(ps3 >> 24), std::uint8_t(ps3 >> 16), std::uint8_t(ps3 >> 8), std::uint8_t(ps3)};'''
else:
    pack='pixels[i] = {std::uint8_t(r), std::uint8_t(g), std::uint8_t(b), std::uint8_t(a)};'
out='''#include "ConsoleLightmap.h"
#include "Mth.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace console {
'''
if not ref:
    out+='''namespace {
void fraction(float value) { if (!std::isfinite(value) || value < 0 || value > 1) throw std::invalid_argument("Invalid lightmap fraction"); }
void validDimension(int value) { if (value < -1 || value > 1) throw std::invalid_argument("Invalid lightmap dimension"); }
}
'''
out+='float consoleTimeOfDay(std::int64_t time,float a,int dimension) {\n'
if not ref:out+='fraction(a);validDimension(dimension);\n'
out+='if(dimension == -1)return 0.5f; if(dimension == 1)return 0.0f;\n'+time+'}\n'
out+='float consoleSkyDarken(std::int64_t time,float rain,float thunder,float a,int dimension) {\n'
if not ref:out+='fraction(rain);fraction(thunder);\n'
out+=sky+'}\n'
out+='float consoleNightVisionScale(int duration,float a) {\n'
if not ref:out+='fraction(a);if(duration < 0)throw std::invalid_argument("Negative night vision duration");\n'
out+=night+'}\n'
out+='void LightFlicker::tick(const std::array<double,8>& randomValues) {\n'
if not ref:out+='for(double value:randomValues)if(!std::isfinite(value) || value < 0 || value >= 1)throw std::invalid_argument("Invalid flicker random sample");\n'
out+=flicker+'}\n'
out+='Lightmap buildConsoleLightmap(const LightmapInput& input) {\n'
if not ref:out+='''validDimension(input.dimension);fraction(input.skyDarken);fraction(input.partialTick);
    if(!std::isfinite(input.flicker) || std::abs(input.flicker)>10 || input.nightVisionTicks < -1)throw std::invalid_argument("Invalid lightmap effect input");
'''
out+='float brightnessRamp[16];\n'+ramp+'\nconst float skyDarken1=input.skyDarken, blr=input.flicker;Lightmap pixels;\nfor(int i=0;i<256;++i){\n'+loop+pack+'\n}\nreturn pixels;\n}\n}\n'
destination=Path(sys.argv[1]);destination.parent.mkdir(parents=True,exist_ok=True);destination.write_text(out)
