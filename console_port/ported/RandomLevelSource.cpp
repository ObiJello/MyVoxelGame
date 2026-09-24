#include "stdafx.h"
#include "RandomLevelSource.h"
#include "Mth.h"

void RandomLevelSource::prepareHeights(int xOffs, int zOffs, byteArray blocks)
{

    int xChunks = 16 / CHUNK_WIDTH;
	int yChunks = Level::genDepth / CHUNK_HEIGHT;
	int waterHeight = level->seaLevel;

    int xSize = xChunks + 1;
    int ySize = Level::genDepth / CHUNK_HEIGHT + 1;
    int zSize = xChunks + 1;

	BiomeArray biomes;	// 4J created locally here for thread safety, java has this as a class member

	level->getBiomeSource()->getRawBiomeBlock(biomes, xOffs * CHUNK_WIDTH - 2, zOffs * CHUNK_WIDTH - 2, xSize + 5, zSize + 5);

	doubleArray buffer;	// 4J - used to be declared with class level scope but tidying up for thread safety reasons
    buffer = getHeights(buffer, xOffs * xChunks, 0, zOffs * xChunks, xSize, ySize, zSize, biomes);

    for (int xc = 0; xc < xChunks; xc++)
	{
        for (int zc = 0; zc < xChunks; zc++)
		{
            for (int yc = 0; yc < yChunks; yc++)
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
						offs -= step;
                        double zStep = 1 / (double) CHUNK_WIDTH;

                        double val = _s0;
                        double vala = (_s1 - _s0) * zStep;
						val -= vala;
                        for (int z = 0; z < CHUNK_WIDTH; z++)
						{
							///////////////////////////////////////////////////////////////////
							// 4J - add this chunk of code to make land "fall-off" at the edges of
							// a finite world - size of that world is currently hard-coded in here
							const int worldSize = m_XZSize * 16;
							const int falloffStart = 32;			// chunks away from edge were we start doing fall-off
							const float falloffMax = 128.0f;			// max value we need to get to falloff by the edge of the map
							
							int xxx = ( ( xOffs * 16 ) + x + ( xc * CHUNK_WIDTH ) );
							int zzz = ( ( zOffs * 16 ) + z + ( zc * CHUNK_WIDTH ) );

							// Get distance to edges of world in x
							int xxx0 = xxx + ( worldSize / 2 );
							if( xxx0 < 0 ) xxx0 = 0;
							int xxx1 = ( ( worldSize / 2 ) - 1 ) - xxx;
							if( xxx1 < 0 ) xxx1 = 0;

							// Get distance to edges of world in z
							int zzz0 = zzz + ( worldSize / 2 );
							if( zzz0 < 0 ) zzz0 = 0;
							int zzz1 = ( ( worldSize / 2 ) - 1 ) - zzz;
							if( zzz1 < 0 ) zzz1 = 0;

							// Get min distance to any edge
							int emin = xxx0;
							if (xxx1 < emin ) emin = xxx1;
							if (zzz0 < emin ) emin = zzz0;
							if (zzz1 < emin ) emin = zzz1;

							float comp = 0.0f;

							// Calculate how much we want the world to fall away, if we're in the defined region to do so
							if( emin < falloffStart )
							{
								int falloff = falloffStart - emin;
								comp = ((float)falloff / (float)falloffStart ) * falloffMax;
							}
							// 4J - end of extra code
							///////////////////////////////////////////////////////////////////

							// 4J - slightly rearranged this code (as of java 1.0.1 merge) to better fit with
							// changes we've made edge-of-world things - original sets blocks[offs += step] directly
							// here rather than setting a tileId
							int tileId = 0;
							// 4J - this comparison used to just be with 0.0f but is now varied by block above
                            if ((val += vala) > comp)
							{
                                tileId = (byte) Tile::rock_Id;
                            }
							else if (yc * CHUNK_HEIGHT + y < waterHeight)
							{
                                tileId = (byte) Tile::calmWater_Id;
                            }

							// 4J - more extra code to make sure that the column at the edge of the world is just water & rock, to match the infinite sea that
							// continues on after the edge of the world.

							if( emin == 0 )
							{
								// This matches code in MultiPlayerChunkCache that makes the geometry which continues at the edge of the world
								if( yc * CHUNK_HEIGHT + y <= ( level->getSeaLevel() - 10 ) ) tileId = Tile::rock_Id;
								else if( yc * CHUNK_HEIGHT + y < level->getSeaLevel() ) tileId = Tile::calmWater_Id;
							}

							blocks[offs += step] = tileId;
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







	// buffer is released by the portable owning array.
	delete [] biomes.data;


}

void RandomLevelSource::buildSurfaces(int xOffs, int zOffs, byteArray blocks, BiomeArray biomes)
{
    int waterHeight = level->seaLevel;

    double s = 1 / 32.0;

	doubleArray depthBuffer(16*16); // 4J - used to be declared with class level scope but moved here for thread safety

    depthBuffer = perlinNoise3->getRegion(depthBuffer, xOffs * 16, zOffs * 16, 0, 16, 16, 1, s * 2, s * 2, s * 2);

    for (int x = 0; x < 16; x++)
	{
        for (int z = 0; z < 16; z++)
		{
            Biome *b = biomes[z + x * 16];
			float temp = b->getTemperature();
            int runDepth = (int) (depthBuffer[x + z * 16] / 3 + 3 + random->nextDouble() * 0.25);

            int run = -1;

            byte top = b->topMaterial;
            byte material = b->material;

			LevelGenerationOptions *lgo = app.getLevelGenerationOptions();
			if(lgo != NULL)
			{
				lgo->getBiomeOverride(b->id,material,top);
			}

            for (int y = Level::genDepthMinusOne; y >= 0; y--)
			{
                int offs = (z * 16 + x) * Level::genDepth + y;

				if (y <= 1 + random->nextInt(2))	// 4J - changed to make the bedrock not have bits you can get stuck in
//                if (y <= 0 + random->nextInt(5))
				{
                    blocks[offs] = (byte) Tile::unbreakable_Id;
                }
				else
				{
                    int old = blocks[offs];

                    if (old == 0)
					{
                        run = -1;
                    }
					else if (old == Tile::rock_Id)
					{
                        if (run == -1)
						{
                            if (runDepth <= 0)
							{
                                top = 0;
                                material = (byte) Tile::rock_Id;
                            }
							else if (y >= waterHeight - 4 && y <= waterHeight + 1)
							{
                                top = b->topMaterial;
								material = b->material;
								if(lgo != NULL)
								{
									lgo->getBiomeOverride(b->id,material,top);
								}
                            }

                            if (y < waterHeight && top == 0)
							{
                                if (temp < 0.15f) top = (byte) Tile::ice_Id;
                                else top = (byte) Tile::calmWater_Id;
                            }

                            run = runDepth;
                            if (y >= waterHeight - 1) blocks[offs] = top;
                            else blocks[offs] = material;
                        } 
						else if (run > 0)
						{
                            run--;
                            blocks[offs] = material;

                            // place a few sandstone blocks beneath sand
                            // runs
                            if (run == 0 && material == Tile::sand_Id)
							{
                                run = random->nextInt(4);
                                material = (byte) Tile::sandStone_Id;
                            }
                        }
                    }
                }
            }
        }
    }

	// depthBuffer is released by the portable owning array.

}

doubleArray RandomLevelSource::getHeights(doubleArray buffer, int x, int y, int z, int xSize, int ySize, int zSize, BiomeArray& biomes)
{
    if (buffer.data == NULL)
	{
        buffer = doubleArray(xSize * ySize * zSize);
    }
	if (pows.data == NULL)
	{
		pows = floatArray(5 * 5);
		for (int xb = -2; xb <= 2; xb++)
		{
			for (int zb = -2; zb <= 2; zb++)
			{
				float ppp = 10.0f / Mth::sqrt(xb * xb + zb * zb + 0.2f);
				pows[xb + 2 + (zb + 2) * 5] = ppp;
			}
		}
	}

    double s = 1 * 684.412;
    double hs = 1 * 684.412;

	doubleArray pnr, ar, br, sr, dr, fi, fis;	// 4J - used to be declared with class level scope but moved here for thread safety

    if (FLOATING_ISLANDS)
	{
        fis = floatingIslandScale->getRegion(fis, x, y, z, xSize, 1, zSize, 1.0, 0, 1.0);
        fi = floatingIslandNoise->getRegion(fi, x, y, z, xSize, 1, zSize, 500.0, 0, 500.0);
    }

#if defined __PS3__ && !defined DISABLE_SPU_CODE
	C4JSpursJobQueue::Port port("C4JSpursJob_PerlinNoise");
	C4JSpursJob_PerlinNoise perlinJob1(&g_scaleNoise_SPU);
	C4JSpursJob_PerlinNoise perlinJob2(&g_depthNoise_SPU);
	C4JSpursJob_PerlinNoise perlinJob3(&g_perlinNoise1_SPU);
	C4JSpursJob_PerlinNoise perlinJob4(&g_lperlinNoise1_SPU);
	C4JSpursJob_PerlinNoise perlinJob5(&g_lperlinNoise2_SPU);

	g_scaleNoise_SPU.set(scaleNoise, sr, x, z, xSize, zSize, 1.121, 1.121, 0.5);
	g_depthNoise_SPU.set(depthNoise, dr, x, z, xSize, zSize, 200.0, 200.0, 0.5);
	g_perlinNoise1_SPU.set(perlinNoise1, pnr, x, y, z, xSize, ySize, zSize, s / 80.0, hs / 160.0, s / 80.0);
	g_lperlinNoise1_SPU.set(lperlinNoise1, ar, x, y, z, xSize, ySize, zSize, s, hs, s);
	g_lperlinNoise2_SPU.set(lperlinNoise2, br, x, y, z, xSize, ySize, zSize, s, hs, s);

	port.submitJob(&perlinJob1);
	port.submitJob(&perlinJob2);
	port.submitJob(&perlinJob3);
	port.submitJob(&perlinJob4);
	port.submitJob(&perlinJob5);
	port.waitForCompletion();
 #else
    sr = scaleNoise->getRegion(sr, x, z, xSize, zSize, 1.121, 1.121, 0.5);
    dr = depthNoise->getRegion(dr, x, z, xSize, zSize, 200.0, 200.0, 0.5);
    pnr = perlinNoise1->getRegion(pnr, x, y, z, xSize, ySize, zSize, s / 80.0, hs / 160.0, s / 80.0);
    ar = lperlinNoise1->getRegion(ar, x, y, z, xSize, ySize, zSize, s, hs, s);
    br = lperlinNoise2->getRegion(br, x, y, z, xSize, ySize, zSize, s, hs, s);

#endif

	x = z = 0;

    int p = 0;
    int pp = 0;

    for (int xx = 0; xx < xSize; xx++)
	{
        for (int zz = 0; zz < zSize; zz++)
		{
			float sss = 0;
			float ddd = 0;
			float pow = 0;

			int rr = 2;

			Biome *mb = biomes[(xx + 2) + (zz + 2) * (xSize + 5)];
			for (int xb = -rr; xb <= rr; xb++)
			{
				for (int zb = -rr; zb <= rr; zb++)
				{
					Biome *b = biomes[(xx + xb + 2) + (zz + zb + 2) * (xSize + 5)];
					float ppp = pows[xb + 2 + (zb + 2) * 5] / (b->depth + 2);
					if (b->depth > mb->depth)
					{
						ppp /= 2;
					}
					sss += b->scale * ppp;
					ddd += b->depth * ppp;
					pow += ppp;
				}
			}
			sss /= pow;
			ddd /= pow;

			sss = sss * 0.9f + 0.1f;
			ddd = (ddd * 4 - 1) / 8.0f;
			
            double rdepth = (dr[pp] / 8000.0);
            if (rdepth < 0) rdepth = -rdepth * 0.3;
            rdepth = rdepth * 3.0 - 2.0;

            if (rdepth < 0)
			{
				rdepth = rdepth / 2;
                if (rdepth < -1) rdepth = -1;
                rdepth = rdepth / 1.4;
                rdepth /= 2;
            } 
			else
			{
                if (rdepth > 1) rdepth = 1;
                rdepth = rdepth / 8;
            }

            pp++;

            for (int yy = 0; yy < ySize; yy++)
			{
				double depth = ddd;
				double scale = sss;

				depth += rdepth * 0.2;
				depth = depth * ySize / 16.0;

				double yCenter = ySize / 2.0 + depth * 4;

                double val = 0;

				double yOffs = (yy - (yCenter)) * 12 * 128 / Level::genDepth / scale;

                if (yOffs < 0) yOffs *= 4;

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

                buffer[p] = val;
                p++;
            }
        }
    }

	// pnr is released by the portable owning array.
	// ar is released by the portable owning array.
	// br is released by the portable owning array.
	// sr is released by the portable owning array.
	// dr is released by the portable owning array.
	// fi is released by the portable owning array.
	// fis is released by the portable owning array.

    return buffer;

}
