#!/usr/bin/env python3
"""Extract unchanged density/surface function bodies for the reference executable.

The platform timing counters are removed; numeric algorithms are not transformed.
The full original source and its hash are retained in the import manifest.
"""
from pathlib import Path
import re
import sys

root=Path(__file__).resolve().parents[1]
source=(root/'original/reference-only/RandomLevelSource.cpp').read_text()
chunks=[]
for start,end in [('void RandomLevelSource::prepareHeights(', 'void RandomLevelSource::buildSurfaces('),
                  ('void RandomLevelSource::buildSurfaces(', 'LevelChunk *RandomLevelSource::create('),
                  ('doubleArray RandomLevelSource::getHeights(', 'bool RandomLevelSource::hasChunk(')]:
    code=source[source.index(start):source.index(end)].rstrip()
    code=re.sub(r'^\s*(?:LARGE_INTEGER (?:startTime|endTime|timeInFunc);|QueryPerformanceCounter\([^;]+;|timeInFunc\.QuadPart[^;]+;|g_numPrepareHeightCalls\+\+;|g_totalPrepareHeightsTime\.QuadPart[^;]+;|g_averagePrepareHeightsTime\.QuadPart[^;]+;)\s*$', '',code,flags=re.MULTILINE)
    chunks.append(code)
out=Path(sys.argv[1]);out.parent.mkdir(parents=True,exist_ok=True)
out.write_text('#include "stdafx.h"\n#include "RandomLevelSource.h"\n#include "Mth.h"\n\n'+'\n\n'.join(chunks)+'\n')
for name in ['LargeCaveFeature','CanyonFeature']:
    original=(root/'original/Minecraft.World'/(name+'.cpp')).read_text()
    # Original LevelType imports std; disambiguate its pre-C++17 byte alias.
    (out.parent/(name+'.cpp')).write_text(original.replace('(byte)', '(::byte)'))
