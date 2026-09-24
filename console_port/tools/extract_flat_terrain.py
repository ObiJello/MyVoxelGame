#!/usr/bin/env python3
"""Extract the original four-layer flat overworld terrain."""
from pathlib import Path
import sys
r=Path(__file__).resolve().parents[1]
s=(r/'original/reference-only/FlatLevelSource.cpp').read_text()
start=s.index('{',s.index('void FlatLevelSource::prepareHeights'))+1;end=s.index('\n}',start)
body=s[start:end].replace('blocks.length','blocks.size()').replace('Tile::unbreakable_Id','7').replace('Tile::dirt_Id','3').replace('Tile::grass_Id','2').replace('(byte) block','static_cast<std::uint8_t>(block)')
Path(sys.argv[1]).write_text('// Adapted from FlatLevelSource::prepareHeights; fixed plains biome from Dimension::init.\n#include "ChunkGenerator.h"\nnamespace console {\nGeneratedChunk generateFlatChunk(){\n GeneratedChunk chunk;auto& blocks=chunk.blocks;\n'+body+'\n chunk.biomes.fill(1);return chunk;\n}\n}\n')
