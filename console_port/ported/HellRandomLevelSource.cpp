#include "HellRandomLevelSource.h"
#include "metadata/ConsoleWorldLimits.h"
using std::min;
HellRandomLevelSource::HellRandomLevelSource(Level *level, __int64 seed)
{
	int xzSize = level->getLevelData()->getXZSize();
	int hellScale = level->getLevelData()->getHellScale();
	m_XZSize = ceil((float)xzSize / hellScale);



    this->level = level;

    random.reset(new Random(seed));
	pprandom.reset(new Random(seed));	// 4J - added, so that we can have a separate random for doing post-processing in parallel with creation
    lperlinNoise1.reset(new PerlinNoise(random.get(), 16));
    lperlinNoise2.reset(new PerlinNoise(random.get(), 16));
    perlinNoise1.reset(new PerlinNoise(random.get(), 8));
    perlinNoise2.reset(new PerlinNoise(random.get(), 4));
    perlinNoise3.reset(new PerlinNoise(random.get(), 4));

    scaleNoise.reset(new PerlinNoise(random.get(), 10));
    depthNoise.reset(new PerlinNoise(random.get(), 16));
}


void HellRandomLevelSource::prepareHeights(int xOffs, int zOffs, byteArray blocks)
{
    int xChunks = 16 / CHUNK_WIDTH;
    int waterHeight = 32;

    int xSize = xChunks + 1;
    int ySize = Level::genDepth / CHUNK_HEIGHT + 1;
    int zSize = xChunks + 1;
	doubleArray buffer;	// 4J - used to be declared with class level scope but tidying up for thread safety reasons
    buffer = getHeights(buffer, xOffs * xChunks, 0, zOffs * xChunks, xSize, ySize, zSize);

    for (int xc = 0; xc < xChunks; xc++)
	{
        for (int zc = 0; zc < xChunks; zc++)
		{
            for (int yc = 0; yc < Level::genDepth / CHUNK_HEIGHT; yc++)
			{
                double yStep = 1 / (double) CHUNK_HEIGHT;
                double s0 = buffer[((xc + 0) * zSize + (zc + 0)) * ySize + (yc + 0)];
                double s1 = buffer[((xc + 0) * zSize + (zc + 1)) * ySize + (yc + 0)];
                double s2 = buffer[((xc + 1) * zSize + (zc + 0)) * ySize + (yc + 0)];
                double s3 = buffer[((xc + 1) * zSize + (zc + 1)) * ySize + (yc + 0)];

                double s0a = (buffer[((xc + 0) * zSize + (zc + 0)) * ySize + (yc + 1)] - s0) * yStep;
                double s1a = (buffer[((xc + 0) * zSize + (zc + 1)) * ySize + (yc + 1)] - s1) * yStep;
                double s2a = (buffer[((xc + 1) * zSize + (zc + 0)) * ySize + (yc + 1)] - s2) * yStep;
                double s3a = (buffer[((xc + 1) * zSize + (zc + 1)) * ySize + (yc + 1)] - s3) * yStep;

                for (int y = 0; y < CHUNK_HEIGHT; y++)
				{
                    double xStep = 1 / (double) CHUNK_WIDTH;

                    double _s0 = s0;
                    double _s1 = s1;
                    double _s0a = (s2 - s0) * xStep;
                    double _s1a = (s3 - s1) * xStep;

                    for (int x = 0; x < CHUNK_WIDTH; x++)
					{
                        int offs = (x + xc * CHUNK_WIDTH) << Level::genDepthBitsPlusFour | (0 + zc * CHUNK_WIDTH) << Level::genDepthBits | (yc * CHUNK_HEIGHT + y);
                        int step = 1 << Level::genDepthBits;
                        double zStep = 1 / (double) CHUNK_WIDTH;

                        double val = _s0;
                        double vala = (_s1 - _s0) * zStep;
                        for (int z = 0; z < CHUNK_WIDTH; z++)
						{
                            int tileId = 0;
                            if (yc * CHUNK_HEIGHT + y < waterHeight)
							{
                                tileId = Tile::calmLava_Id;
                            }
                            if (val > 0)
							{
                                tileId = Tile::hellRock_Id;
                            }

                            blocks[offs] = (::byte) tileId;
                            offs += step;
                            val += vala;
                        }
                        _s0 += _s0a;
                        _s1 += _s1a;
                    }

                    s0 += s0a;
                    s1 += s1a;
                    s2 += s2a;
                    s3 += s3a;
                }
            }
        }
    }

}


void HellRandomLevelSource::buildSurfaces(int xOffs, int zOffs, byteArray blocks)
{
    int waterHeight = Level::genDepth - 64;

    double s = 1 / 32.0;

    doubleArray sandBuffer(16*16);	// 4J - used to be declared with class level scope but moved here for thread safety
	doubleArray gravelBuffer(16*16);
	doubleArray depthBuffer(16*16);

    sandBuffer = perlinNoise2->getRegion(sandBuffer, xOffs * 16, zOffs * 16, 0, 16, 16, 1, s, s, 1);
    gravelBuffer = perlinNoise2->getRegion(gravelBuffer, xOffs * 16, 109, zOffs * 16, 16, 1, 16, s, 1, s);
    depthBuffer = perlinNoise3->getRegion(depthBuffer, xOffs * 16, zOffs * 16, 0, 16, 16, 1, s * 2, s * 2, s * 2);

    for (int x = 0; x < 16; x++)
	{
        for (int z = 0; z < 16; z++)
		{
            bool sand = (sandBuffer[x + z * 16] + random->nextDouble() * 0.2) > 0;
            bool gravel = (gravelBuffer[x + z * 16] + random->nextDouble() * 0.2) > 0;
            int runDepth = (int) (depthBuffer[x + z * 16] / 3 + 3 + random->nextDouble() * 0.25);

            int run = -1;

            ::byte top = (::byte) Tile::hellRock_Id;
            ::byte material = (::byte) Tile::hellRock_Id;

            for (int y = Level::genDepthMinusOne; y >= 0; y--)
			{
                int offs = (z * 16 + x) * Level::genDepth + y;

				// 4J Build walls around the level
				bool blockSet = false;
				if(xOffs <= -(m_XZSize/2))
				{
					if( z - random->nextInt( 4 ) <= 0 || xOffs < -(m_XZSize/2) )
					{
						blocks[offs] = (::byte) Tile::unbreakable_Id;
						blockSet = true;
					}
				}
				if(zOffs <= -(m_XZSize/2))
				{
					if( x - random->nextInt( 4 ) <= 0 || zOffs < -(m_XZSize/2))
					{
						blocks[offs] = (::byte) Tile::unbreakable_Id;
						blockSet = true;
					}
				}
				if(xOffs >= (m_XZSize/2)-1)
				{
					if( z + random->nextInt(4) >= 15 || xOffs > (m_XZSize/2))
					{
						blocks[offs] = (::byte) Tile::unbreakable_Id;
						blockSet = true;
					}
				}
				if(zOffs >= (m_XZSize/2)-1)
				{
					if( x + random->nextInt(4) >= 15 || zOffs > (m_XZSize/2) )
					{
						blocks[offs] = (::byte) Tile::unbreakable_Id;
						blockSet = true;
					}
				}
				if( blockSet ) continue;
				// End 4J Extra to build walls around the level

				if (y >= Level::genDepthMinusOne - random->nextInt(5) || y <= 0 + random->nextInt(5))
				{
                    blocks[offs] = (::byte) Tile::unbreakable_Id;
                }
				else
				{
                    int old = blocks[offs];

                    if (old == 0)
					{
                        run = -1;
                    }
					else if (old == Tile::hellRock_Id)
					{
                        if (run == -1)
						{
                            if (runDepth <= 0)
							{
                                top = 0;
                                material = (::byte) Tile::hellRock_Id;
                            }
							else if (y >= waterHeight - 4 && y <= waterHeight + 1)
							{
                                top = (::byte) Tile::hellRock_Id;
                                material = (::byte) Tile::hellRock_Id;
                                if (gravel) top = (::byte) Tile::gravel_Id;
                                if (gravel) material = (::byte) Tile::hellRock_Id;
								if (sand)
								{
									// 4J Stu - Make some nether wart spawn outside of the nether fortresses
									if(random->nextInt(16) == 0)
									{
										top = (::byte) Tile::netherStalk_Id;
										
										// Place the nether wart on top of the soul sand
										y += 1;
										int genDepthMinusOne = Level::genDepthMinusOne; // Take into local int for PS4 as min takes a reference to the const int there and then needs the value to exist for the linker
										y = min(y, genDepthMinusOne);
										runDepth += 1;
										offs = (z * 16 + x) * Level::genDepth + y;
									}
									else
									{
										top = (::byte) Tile::hellSand_Id;
									}
								}
                                if (sand) material = (::byte) Tile::hellSand_Id;
                            }

                            if (y < waterHeight && top == 0) top = (::byte) Tile::calmLava_Id;

                            run = runDepth;
							// 4J Stu - If sand, then allow adding nether wart at heights below the water level
                            if (y >= waterHeight - 1 || sand) blocks[offs] = top;
                            else blocks[offs] = material;
                        }
						else if (run > 0)
						{
                            run--;
                            blocks[offs] = material;
                        }
                    }
                }
            }
        }
    }



}


doubleArray HellRandomLevelSource::getHeights(doubleArray buffer, int x, int y, int z, int xSize, int ySize, int zSize)
{
    if (buffer.data == NULL)
	{
        buffer = doubleArray(xSize * ySize * zSize);
    }

    double s = 1 * 684.412;
    double hs = 1 * 684.412 * 3;

	doubleArray pnr, ar, br, sr, dr, fi, fis;	// 4J - used to be declared with class level scope but moved here for thread safety

    sr = scaleNoise->getRegion(sr, x, y, z, xSize, 1, zSize, 1.0, 0, 1.0);
    dr = depthNoise->getRegion(dr, x, y, z, xSize, 1, zSize, 100.0, 0, 100.0);

    pnr = perlinNoise1->getRegion(pnr, x, y, z, xSize, ySize, zSize, s / 80.0, hs / 60.0, s / 80.0);
    ar = lperlinNoise1->getRegion(ar, x, y, z, xSize, ySize, zSize, s, hs, s);
    br = lperlinNoise2->getRegion(br, x, y, z, xSize, ySize, zSize, s, hs, s);

    int p = 0;
    int pp = 0;
    doubleArray yoffs = doubleArray(ySize);
    for (int yy = 0; yy < ySize; yy++)
	{
        yoffs[yy] = cos(yy * PI * 6 / (double) ySize) * 2;

        double dd = yy;
        if (yy > ySize / 2)
		{
            dd = (ySize - 1) - yy;
        }
        if (dd < 4) {
            dd = 4 - dd;
            yoffs[yy] -= dd * dd * dd * 10;
        }
    }

    for (int xx = 0; xx < xSize; xx++)
	{
        for (int zz = 0; zz < zSize; zz++)
		{
            double scale = ((sr[pp] + 256.0) / 512);
            if (scale > 1) scale = 1;

            double floating = 0;

            double depth = (dr[pp] / 8000.0);
            if (depth < 0) depth = -depth;
            depth = depth * 3.0 - 3.0;

            if (depth < 0)
			{
                depth = depth / 2;
                if (depth < -1) depth = -1;
                depth = depth / 1.4;
                depth /= 2;
                scale = 0;
            }
			else
			{
                if (depth > 1) depth = 1;
                depth = depth / 6;
            }
            scale = (scale) + 0.5;
            depth = depth * ySize / 16;
            pp++;

            for (int yy = 0; yy < ySize; yy++)
			{
                double val = 0;

                double yOffs = yoffs[yy];

                double bb = ar[p] / 512;
                double cc = br[p] / 512;

                double v = (pnr[p] / 10 + 1) / 2;
                if (v < 0) val = bb;
                else if (v > 1) val = cc;
                else val = bb + (cc - bb) * v;
                val -= yOffs;

                if (yy > ySize - 4)
				{
                    double slide = (yy - (ySize - 4)) / (4 - 1.0f);
                    val = val * (1 - slide) + -10 * slide;
                }

                if (yy < floating)
				{
                    double slide = (floating - yy) / (4);
                    if (slide < 0) slide = 0;
                    if (slide > 1) slide = 1;
                    val = val * (1 - slide) + -10 * slide;
                }

                buffer[p] = val;
                p++;
            }
        }
    }









    return buffer;
}

