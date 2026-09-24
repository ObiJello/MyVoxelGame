#!/usr/bin/env python3
"""Extract the archived chunk header and eight-section write order, without entities."""
from pathlib import Path
import sys
r=Path(__file__).resolve().parents[1]
s=(r/'original/reference-only/OldChunkStorage.cpp').read_text()
a=s.index('void OldChunkStorage::save(LevelChunk *lc, Level *level, DataOutputStream *dos)');b=s.index('\n\tPIXBeginNamedEvent(0,"Saving entities");',a)
s=s[a:b];s=s[s.index('{')+1:]
s='\n'.join(line for line in s.splitlines() if not line.strip().startswith(('PIXBeginNamedEvent','PIXEndNamedEvent')))
s=s.replace('lc->','this->').replace('level->getTime()','lastUpdate')
out='#include "ChunkRecord.h"\nvoid console::ChunkRecord::writePrefix(DataOutputStream* dos)\n{'+s+'\n}\n'
source=(r/'original/reference-only/LevelChunk.cpp').read_text()
for method in ['writeCompressedBlockData','writeCompressedDataData','writeCompressedSkyLightData','writeCompressedBlockLightData']:
    a=source.index('void LevelChunk::'+method+'(');b=source.index('{',a)+1;depth=1
    while depth:depth+=(source[b]=='{')-(source[b]=='}');b+=1
    out+=source[a:b].replace('LevelChunk::','console::ChunkRecord::')+'\n'
Path(sys.argv[1]).write_text('// Original numeric fields and section order; entity construction is a separate boundary.\n'+out)
