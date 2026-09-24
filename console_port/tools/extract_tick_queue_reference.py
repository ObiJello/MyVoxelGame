#!/usr/bin/env python3
"""Extract original ServerLevel scheduling, replacing only world/platform hooks."""
from pathlib import Path
import re,sys
r=Path(__file__).resolve().parents[1]
s=(r/'original/reference-only/ServerLevel.cpp').read_text()
out='#include "ScheduledTickQueue.h"\n#include "stdafx.h"\n#define AUTO_VAR(name,value) auto name = value\nnamespace console {\n'
methods=['void ServerLevel::addToTickNextTick(', 'void ServerLevel::forceAddTileTick(',
         'bool ServerLevel::tickPendingTicks(', 'vector<TickNextTickData> *ServerLevel::fetchTicksInChunk(',
         'void ServerLevel::setTimeAndAdjustTileTicks(']
for signature in methods:
    a=s.index(signature);b=s.index('{',a)+1;depth=1
    while depth:
        depth+=(s[b]=='{')-(s[b]=='}');b+=1
    body=s[a:b].replace('ServerLevel::','ScheduledTickQueue::')
    body=body.replace('EnterCriticalSection(&m_tickNextTickCS);','std::lock_guard lock(m_tickNextTickCS);')
    body=body.replace('LeaveCriticalSection(&m_tickNextTickCS);','')
    body=body.replace('levelData->getTime()', 'host.getTime()')
    for name in ['getInstaTick','hasChunksAt','getTile','setTime']:
        body=re.sub(r'\b'+name+r'\(', 'host.'+name+'(',body)
    body=body.replace('Tile::tiles[id]->tick(this, td.x, td.y, td.z, random);','host.tickTile(id, td.x, td.y, td.z);')
    if 'fetchTicksInChunk' in signature:
        body=body.replace('vector<TickNextTickData> *ScheduledTickQueue::fetchTicksInChunk(LevelChunk *chunk, bool remove)',
                          'std::vector<TickNextTickData> ScheduledTickQueue::fetchTicksInChunk(int chunkX, int chunkZ, bool remove)')
        body=body.replace('vector<TickNextTickData> *results = new vector<TickNextTickData>;', 'std::vector<TickNextTickData> results;')
        start=body.index('ChunkPos *pos = chunk->getPos();');end=body.index('delete pos;',start)+len('delete pos;')
        body=body[:start]+'std::int64_t west=std::int64_t(chunkX)*16, east=west+16, north=std::int64_t(chunkZ)*16, south=north+16;'+body[end:]
        body=body.replace('results->push_back','results.push_back')
    out+=body+'\n'
Path(sys.argv[1]).write_text(out+'}\n')
