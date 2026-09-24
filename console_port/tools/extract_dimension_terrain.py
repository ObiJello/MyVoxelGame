#!/usr/bin/env python3
"""Extract original Nether/End numeric stages, preserving original reference loops."""
from pathlib import Path
import re,sys
root=Path(__file__).resolve().parents[1];out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
reference='--reference' in sys.argv
for name in ['HellRandomLevelSource','TheEndLevelRandomLevelSource']:
 s=(root/'original/reference-only'/(name+'.cpp')).read_text()
 ctor=s[s.index(name+'::'+name+'('):s.index(name+'::~'+name+'(')]
 ctor=re.sub(r'^\s*(netherBridgeFeature|caveFeature) = new [^;]+;','',ctor,flags=re.M)
 # Platform-bound objects are outside this subset; all noise creation statements
 # remain in their original order. Unique ownership makes partial construction safe.
 ctor=re.sub(r'\b(\w+) = new ([^;]+);',r'\1.reset(new \2);',ctor).replace('PerlinNoise(random,','PerlinNoise(random.get(),')
 pieces=[ctor]
 for start,end in [('void '+name+'::prepareHeights(', 'void '+name+'::buildSurfaces('),('void '+name+'::buildSurfaces(', 'LevelChunk *'+name+'::create('),('doubleArray '+name+'::getHeights(', 'bool '+name+'::hasChunk(')]:
  pieces.append(s[s.index(start):s.index(end)])
 text='\n'.join(pieces)
 text=re.sub(r'\bbyte\b','::byte',text)
 if not reference:text=re.sub(r'^\s*delete \[\] \w+\.data;','',text,flags=re.M)
 (out/(name+'.cpp')).write_text('#include "'+name+'.h"\n#include "metadata/ConsoleWorldLimits.h"\nusing std::min;\n'+text)
# No runtime/platform calls occur in the original Nether carver. Keep all branch,
# random and carving decisions intact, with RAII for its recursive local Random.
s=(root/'original/Minecraft.World/LargeHellCaveFeature.cpp').read_text().replace('(byte)','(::byte)')
if not reference:
 s=s.replace('Random *random = new Random(this->random->nextLong());','auto random = std::make_unique<Random>(this->random->nextLong());')
 s=re.sub(r'^\s*delete random;','',s,flags=re.M)
(out/'LargeHellCaveFeature.cpp').write_text(s)
