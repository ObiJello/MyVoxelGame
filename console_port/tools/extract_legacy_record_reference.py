#!/usr/bin/env python3
"""Extract the original legacy save fields and raw section export methods."""
from pathlib import Path
import re,sys
r=Path(__file__).resolve().parents[1]
def uncomment(s):
 return re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/',lambda m:m[0] if m[0].startswith('"') else '\n'*m[0].count('\n'),s,flags=re.S)
s=uncomment((r/'original/reference-only/OldChunkStorage.cpp').read_text())
a=s.index('void OldChunkStorage::save(LevelChunk *lc, Level *level, CompoundTag *tag)');a=s.index('{',a)+1;b=s.index('PIXBeginNamedEvent(0,"Saving entities");',a);body=s[a:b]
body=re.sub(r'ThreadStorage \*tls =[^;]*;','',body).replace('level->checkSession();','').replace('level->getTime()','lc->lastUpdate')
body='\n'.join(l for l in body.splitlines() if not l.strip().startswith(('PIXBeginNamedEvent','PIXEndNamedEvent')))
methods=['getBlockData','getDataData','getSkyLightData','getBlockLightData']
out='#include "ChunkRecord.h"\n'
source=uncomment((r/'original/reference-only/LevelChunk.cpp').read_text())
for name in methods:
 a=source.index('void LevelChunk::'+name+'(');a=source.index('{',a)+1;b=source.index('}',a)
 method=source[a:b].replace('Level::COMPRESSED_CHUNK_SECTION_TILES','32768')
 method=re.sub(r'\b(lowerBlocks|upperBlocks|lowerData|upperData|lowerSkyLight|upperSkyLight|lowerBlockLight|upperBlockLight)\b',r'lc->\1',method)
 out+='static void '+name+'(console::ChunkRecord* lc,byteArray data){'+method+'}\n'
 body=body.replace('lc->'+name+'(',name+'(lc,')
out+='''void originalWriteLegacy(console::ChunkRecord* lc,DataOutputStream* output){
    struct Buffers {byteArray blockData{65536},dataData{32768},skyLightData{32768},blockLightData{32768};
        ~Buffers(){delete[] blockData.data;delete[] dataData.data;delete[] skyLightData.data;delete[] blockLightData.data;}} buffers;
    auto* tls=&buffers;auto owned=std::make_unique<CompoundTag>();auto* tag=owned.get();
'''+body+'''
    CompoundTag root;root.put(L"Level",owned.release());NbtIo::write(&root,output);
}
'''
Path(sys.argv[1]).write_text(out)
