#!/usr/bin/env python3
"""Extract BiomeSource fractional accounting and console seed acceptance rules."""
from pathlib import Path
import sys
r=Path(__file__).resolve().parents[1];source=(r/'original/reference-only/BiomeSource.cpp').read_text()
def body(signature):
 a=source.index('{',source.index(signature))+1;b=a;depth=1
 while depth:
  if source[b]=='{':depth+=1
  if source[b]=='}':depth-=1
  b+=1
 return source[a:b-1].replace('Biome::BIOME_COUNT','23').replace('indices.length','indices.size()')
out='''// Generated from BiomeSource::getFracs and getIsMatch.
#include "ConsoleSeed.h"
#include <cmath>
#include <stdexcept>
namespace console {
std::array<float,23> consoleBiomeFractions(std::span<const std::uint8_t> indices){
 if(indices.empty() || indices.size()>40000)throw std::invalid_argument("Invalid seed biome sample size");
 for(auto id:indices)if(id>=23)throw std::invalid_argument("Invalid seed biome ID");
 std::array<float,23> fracs;
'''+body('void BiomeSource::getFracs(')+'''return fracs;
}
bool consoleSeedMatches(std::array<float,23> frac){
 for(float f:frac)if(!std::isfinite(f) || f<0 || f>1)throw std::invalid_argument("Invalid biome fraction");
'''+body('bool BiomeSource::getIsMatch(')+'\n}\n}\n'
Path(sys.argv[1]).write_text(out)
