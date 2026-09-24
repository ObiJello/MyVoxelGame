#!/usr/bin/env python3
"""Extract the original tick-NBT writer without entity or platform dependencies."""
from pathlib import Path
import sys
r=Path(__file__).resolve().parents[1]
s=(r/'original/reference-only/OldChunkStorage.cpp').read_text()
a=s.index('ListTag<CompoundTag> *tickTags = new ListTag<CompoundTag>();')
b=s.index('tag->put(L"TileTicks", tickTags);',a)+len('tag->put(L"TileTicks", tickTags);')
out='#include "NbtIo.h"\n#include "TickNextTickData.h"\nvoid originalWriteTileTicks(CompoundTag* tag, std::vector<TickNextTickData>* ticksInChunk, std::int64_t levelTime) {\n'
Path(sys.argv[1]).write_text(out+s[a:b]+'\n}\n')
