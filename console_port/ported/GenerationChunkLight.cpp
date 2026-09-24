// Original chunk lighting with flat-storage views.
#include "GenerationChunkLight.h"

void GenerationChunkLight::recalcHeightmap()
{
#ifdef __PSVITA__
	// AP - lets fetch ALL the chunk data at the same time for a good speed up
	byteArray blockData = byteArray(Level::CHUNK_TILE_COUNT);
	getBlockData(blockData);
#endif

    int min = Level::maxBuildHeight;
    for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++)
		{
            int y = Level::maxBuildHeight;
//            int p = x << level->depthBitsPlusFour | z << level->depthBits;			// 4J - removed
			
#ifdef __PSVITA__
			int Index = ( x << 11 ) + ( z << 7 );
			int offset = Level::COMPRESSED_CHUNK_SECTION_TILES;
            y = 127;
			while (y > 0 && Tile::lightBlock[blockData[Index + offset + (y - 1)]] == 0)		// 4J - was blocks->get() was blocks[p + y - 1]
			{
				y--;
			}
			if( y == 0 )
			{
				offset = 0;
				y = 127;
				while (y > 0 && Tile::lightBlock[blockData[Index + offset + (y - 1)]] == 0)		// 4J - was blocks->get() was blocks[p + y - 1]
				{
					y--;
				}
			}
			else
			{
				y += 128;
			}
#else
			CompressedTileStorage *blocks = (y-1) >= Level::COMPRESSED_CHUNK_SECTION_HEIGHT?upperBlocks : lowerBlocks;
            while (y > 0 && Tile::lightBlock[blocks->get(x,(y-1) % Level::COMPRESSED_CHUNK_SECTION_HEIGHT,z) & 0xff] == 0)			// 4J - was blocks->get() was blocks[p + y - 1]
			{
                y--;
				blocks = (y-1) >= Level::COMPRESSED_CHUNK_SECTION_HEIGHT?upperBlocks : lowerBlocks;
			}
#endif
            heightmap[z << 4 | x] = y;
            if (y < min) min = y;

            if (!level->dimension->hasCeiling)
			{
                int br = Level::MAX_BRIGHTNESS;
				int yy = Level::maxBuildHeight - 1;
#ifdef __PSVITA__
				int offset = Level::COMPRESSED_CHUNK_SECTION_TILES;
				SparseLightStorage *skyLight = upperSkyLight;
				yy = 127;
                do
				{
                    br -= Tile::lightBlock[blockData[Index + offset + yy]];					// 4J - blocks->get() was blocks[p + yy]
                    if (br > 0)
					{
                        skyLight->set(x, yy, z, br);
                    }
                    yy--;
                } while (yy > 0 && br > 0);

				if( yy == 0 && br > 0 )
				{
					offset = 0;
					skyLight = lowerSkyLight;
					yy = 127;
					do
					{
						br -= Tile::lightBlock[blockData[Index + offset + yy]];					// 4J - blocks->get() was blocks[p + yy]
						if (br > 0)
						{
							skyLight->set(x, yy, z, br);
						}
						yy--;
					} while (yy > 0 && br > 0);
				}
#else
				CompressedTileStorage *blocks = yy >= Level::COMPRESSED_CHUNK_SECTION_HEIGHT?upperBlocks : lowerBlocks;
				SparseLightStorage *skyLight = yy >= Level::COMPRESSED_CHUNK_SECTION_HEIGHT? upperSkyLight : lowerSkyLight;
                do
				{
                    br -= Tile::lightBlock[blocks->get(x,(yy % Level::COMPRESSED_CHUNK_SECTION_HEIGHT),z) & 0xff];					// 4J - blocks->get() was blocks[p + yy]
                    if (br > 0)
					{
                        skyLight->set(x, (yy % Level::COMPRESSED_CHUNK_SECTION_HEIGHT), z, br);
                    }
                    yy--;
					blocks = yy >= Level::COMPRESSED_CHUNK_SECTION_HEIGHT?upperBlocks : lowerBlocks;
					skyLight = yy >= Level::COMPRESSED_CHUNK_SECTION_HEIGHT? upperSkyLight : lowerSkyLight;
                } while (yy > 0 && br > 0);
#endif
            }
        }

    this->minHeight = min;

    for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++)
		{
            lightGaps(x, z);
        }

    this->setUnsaved(true);

#ifdef __PSVITA__
	delete blockData.data;
#endif
}

void GenerationChunkLight::lightLava()
{
	if( !emissiveAdded ) return;
	
	for (int x = 0; x < 16; x++)
		for (int z = 0; z < 16; z++)
		{
//			int p = x << 11 | z << 7;			// 4J - removed
			int ymax = getHeightmap(x,z);
			for (int y = 0; y < Level::maxBuildHeight; y++)
			{
				CompressedTileStorage *blocks = y < Level::COMPRESSED_CHUNK_SECTION_HEIGHT ? lowerBlocks : upperBlocks;
				int emit = Tile::lightEmission[blocks->get(x,y % Level::COMPRESSED_CHUNK_SECTION_HEIGHT,z)];	// 4J - blocks->get() was blocks[p + y]
				if( emit > 0 )
				{
//					printf("(%d,%d,%d)",this->x * 16 + x, y, this->z * 16 + z);
					// We'll be calling this function for a lot of chunks as they are post-processed. For every chunk that is
					// post-processed we're calling this for each of its neighbours in case some post-processing also created something
					// that needed lighting outside the starting chunk. Because of this, do a quick test on any emissive blocks that have
					// been added to see if checkLight has already been run on this particular block - this is straightforward to check
					// as being emissive blocks they'll have their block brightness set to their lightEmission level in this case.
					if( getBrightness(LightLayer::Block, x, y, z) < emit )
					{
						level->checkLight( LightLayer::Block, this->x * 16 + x, y, this->z * 16 + z, true);
					}
				}
			}
		}
	emissiveAdded = false;
}

void GenerationChunkLight::lightGaps(int x, int z)
{
	// 4J - lighting change brought forward from 1.8.2, introduced an array of bools called gapsToRecheck, which are now a single bit in array of nybbles in this version
	int slot = ( x >> 1 ) | (z * 8);
	int shift = ( x & 1 ) * 4;
	columnFlags[slot] |= ( eColumnFlag_recheck << shift );
	hasGapsToCheck = true;
}

void GenerationChunkLight::recheckGaps(bool bForce)
{
	// 4J added - otherwise we can end up doing a very broken kind of lighting since for an empty chunk, the heightmap is all zero, but it
	// still has an x and z of 0 which means that the level->getHeightmap references in here find a real chunk near the origin, and then attempt
	// to light massive gaps between the height of 0 and whatever heights are in those.
	if( isEmpty() ) return;		

	// 4J added
	int minXZ = - (level->dimension->getXZSize() * 16 ) / 2;
	int maxXZ = (level->dimension->getXZSize() * 16 ) / 2 - 1;

	// 4J - note - this test will currently return true for chunks at the edge of our world. Making further checks inside the loop now to address this issue.
    if (level->hasChunksAt(x * 16 + 8, Level::maxBuildHeight / 2, z * 16 + 8, 16))
	{
        for (int x = 0; x < 16; x++)
            for (int z = 0; z < 16; z++)
			{
				int slot = ( x >> 1 ) | (z * 8);
				int shift = ( x & 1 ) * 4;
                if (bForce || ( columnFlags[slot] & ( eColumnFlag_recheck << shift ) ) )
				{
                    columnFlags[slot] &= ~( eColumnFlag_recheck << shift );
                    int height = getHeightmap(x, z);
                    int xOffs = (this->x * 16) + x;
                    int zOffs = (this->z * 16) + z;

					// 4J - rewritten this to make sure that the minimum neighbour height which is calculated doesn't involve getting any heights from beyond the edge of the world,
					// which can lead to large, very expensive, non-existent cliff edges to be lit
					int nmin = level->getHeightmap(xOffs, zOffs);
					if( xOffs - 1 >= minXZ )
					{
						int n = level->getHeightmap(xOffs - 1, zOffs);
						if ( n < nmin ) nmin = n;
					}
					if( xOffs + 1 <= maxXZ )
					{
						int n = level->getHeightmap(xOffs + 1, zOffs);
						if ( n < nmin ) nmin = n;
					}
					if( zOffs - 1 >= minXZ )
					{
						int n = level->getHeightmap(xOffs, zOffs - 1);
						if ( n < nmin ) nmin = n;
					}
					if( zOffs + 1 <= maxXZ )
					{
						int n = level->getHeightmap(xOffs, zOffs + 1);
						if ( n < nmin ) nmin = n;
					}
                    lightGap(xOffs, zOffs, nmin);

					if( !bForce )	// 4J - if doing a full forced thing over every single column, we don't need to do these offset checks too
					{
						if( xOffs - 1 >= minXZ ) lightGap(xOffs - 1, zOffs, height);
						if( xOffs + 1 <= maxXZ ) lightGap(xOffs + 1, zOffs, height);
						if( zOffs - 1 >= minXZ ) lightGap(xOffs, zOffs - 1, height);
						if( zOffs + 1 <= maxXZ ) lightGap(xOffs, zOffs + 1, height);
					}
					hasGapsToCheck = false;
                }
            }
    }
}

void GenerationChunkLight::lightGap(int x, int z, int source)
{
    int height = level->getHeightmap(x, z);

    if (height > source)
	{
        lightGap(x, z, source, height + 1);
    }
	else if (height < source) 
	{
        lightGap(x, z, height, source + 1);
    }
}

void GenerationChunkLight::lightGap(int x, int z, int y1, int y2)
{
    if (y2 > y1)
	{
        if (level->hasChunksAt(x, Level::maxBuildHeight / 2, z, 16))
		{
            for (int y = y1; y < y2; y++)
			{
                level->checkLight(LightLayer::Sky, x, y, z);
            }
            this->setUnsaved(true);
        }
    }
}

void GenerationChunkLight::recalcHeight(int x, int yStart, int z)
{
    int yOld = heightmap[z << 4 | x];
    int y = yOld;
    if (yStart > yOld) y = yStart;

//    int p = x << level->depthBitsPlusFour | z << level->depthBits;		// 4J - removed
	
	CompressedTileStorage *blocks = (y-1) >= Level::COMPRESSED_CHUNK_SECTION_HEIGHT?upperBlocks : lowerBlocks;
	while (y > 0 && Tile::lightBlock[blocks->get(x,(y-1) % Level::COMPRESSED_CHUNK_SECTION_HEIGHT,z) & 0xff] == 0)		// 4J - blocks->get() was blocks[p + y - 1]
	{
        y--;
		blocks = (y-1) >= Level::COMPRESSED_CHUNK_SECTION_HEIGHT?upperBlocks : lowerBlocks;
	}
    if (y == yOld) return;

//    level->lightColumnChanged(x, z, y, yOld);		// 4J - this call moved below & corrected - see comment further down
    heightmap[z << 4 | x] = y;

    if (y < minHeight)
	{
        minHeight = y;
    }
	else
	{
        int min = Level::maxBuildHeight;
        for (int _x = 0; _x < 16; _x++)
            for (int _z = 0; _z < 16; _z++)
			{
                if ((heightmap[_z << 4 | _x]) < min) min = (heightmap[_z << 4 | _x]);
            }
        this->minHeight = min;
    }

    int xOffs = (this->x * 16) + x;
    int zOffs = (this->z * 16) + z;
	if (!level->dimension->hasCeiling)
	{
		if (y < yOld)
		{
			SparseLightStorage *skyLight = y >= Level::COMPRESSED_CHUNK_SECTION_HEIGHT? upperSkyLight : lowerSkyLight;
			for (int yy = y; yy < yOld; yy++)
			{
				skyLight = yy >= Level::COMPRESSED_CHUNK_SECTION_HEIGHT? upperSkyLight : lowerSkyLight;
				skyLight->set(x, (yy % Level::COMPRESSED_CHUNK_SECTION_HEIGHT), z, 15);
			}
		} else
		{
			// 4J - lighting change brought forward from 1.8.2
	//        level->updateLight(LightLayer::Sky, xOffs, yOld, zOffs, xOffs, y, zOffs);
			SparseLightStorage *skyLight = y >= Level::COMPRESSED_CHUNK_SECTION_HEIGHT? upperSkyLight : lowerSkyLight;
			for (int yy = yOld; yy < y; yy++)
			{
				skyLight = yy >= Level::COMPRESSED_CHUNK_SECTION_HEIGHT? upperSkyLight : lowerSkyLight;
				skyLight->set(x, (yy % Level::COMPRESSED_CHUNK_SECTION_HEIGHT), z, 0);
			}
		}

		int br = 15;
		
		SparseLightStorage *skyLight = y >= Level::COMPRESSED_CHUNK_SECTION_HEIGHT? upperSkyLight : lowerSkyLight;
		while (y > 0 && br > 0)
		{
			y--;
			skyLight = y >= Level::COMPRESSED_CHUNK_SECTION_HEIGHT? upperSkyLight : lowerSkyLight;
			int block = Tile::lightBlock[getTile(x, y, z)];
			if (block == 0) block = 1;
			br -= block;
			if (br < 0) br = 0;
			skyLight->set(x, (y % Level::COMPRESSED_CHUNK_SECTION_HEIGHT), z, br);
			// level.updateLightIfOtherThan(LightLayer.Sky, xOffs, y, zOffs,
	// -1);
		}
	}
	// 4J - changed to use xOffs and zOffs rather than the (incorrect) x and z it used to, and also moved so that it happens after all the lighting should be
	// done by this stage, as this will trigger our asynchronous render updates immediately (potentially) so don't want to say that the lighting is done & then do it
	level->lightColumnChanged(xOffs, zOffs, y, yOld);

	// 4J -  lighting changes brought forward from 1.8.2
    int height = heightmap[z << 4 | x];
    int y1 = yOld;
    int y2 = height;
    if (y2 < y1)
	{
        int tmp = y1;
        y1 = y2;
        y2 = tmp;
    }
	if (!level->dimension->hasCeiling)
	{
		PIXBeginNamedEvent(0,"Light gaps");
		lightGap(xOffs - 1, zOffs, y1, y2);
		lightGap(xOffs + 1, zOffs, y1, y2);
		lightGap(xOffs, zOffs - 1, y1, y2);
		lightGap(xOffs, zOffs + 1, y1, y2);
		lightGap(xOffs, zOffs, y1, y2);
		PIXEndNamedEvent();
	}

    this->setUnsaved(true);
}
