// Adapted from FlatLevelSource::prepareHeights; fixed plains biome from Dimension::init.
#include "ChunkGenerator.h"
namespace console {
GeneratedChunk generateFlatChunk(){
 GeneratedChunk chunk;auto& blocks=chunk.blocks;

	int height = blocks.size() / (16 * 16);

	for (int xc = 0; xc < 16; xc++) 
	{
		for (int zc = 0; zc < 16; zc++) 
		{
			for (int yc = 0; yc < height; yc++) 
			{
				int block = 0;
				if (yc == 0) 
				{
					block = 7;
				} 
				else if (yc <= 2) 
				{
					block = 3;
				} 
				else if (yc == 3) 
				{
					block = 2;
				}
				blocks[xc << 11 | zc << 7 | yc] = static_cast<std::uint8_t>(block);
			}
		}
	}
 chunk.biomes.fill(1);return chunk;
}
}
