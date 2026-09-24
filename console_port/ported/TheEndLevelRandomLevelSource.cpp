#include "TheEndLevelRandomLevelSource.h"
#include "metadata/ConsoleWorldLimits.h"
using std::min;
TheEndLevelRandomLevelSource::TheEndLevelRandomLevelSource(Level *level, __int64 seed)
{
	m_XZSize = END_LEVEL_MIN_WIDTH;

    this->level = level;

    random.reset(new Random(seed));
	pprandom.reset(new Random(seed));	// 4J added
    lperlinNoise1.reset(new PerlinNoise(random.get(), 16));
    lperlinNoise2.reset(new PerlinNoise(random.get(), 16));
    perlinNoise1.reset(new PerlinNoise(random.get(), 8));

    scaleNoise.reset(new PerlinNoise(random.get(), 10));
    depthNoise.reset(new PerlinNoise(random.get(), 16));
}


void TheEndLevelRandomLevelSource::prepareHeights(int xOffs, int zOffs, byteArray blocks, BiomeArray biomes)
{
	doubleArray buffer;	// 4J - used to be declared with class level scope but tidying up for thread safety reasons

    int xChunks = 16 / CHUNK_WIDTH;

    int xSize = xChunks + 1;
    int ySize = Level::genDepth / CHUNK_HEIGHT + 1;
    int zSize = xChunks + 1;
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
                            if (val > 0)
							{
                                tileId = Tile::whiteStone_Id;
                            } else {
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


void TheEndLevelRandomLevelSource::buildSurfaces(int xOffs, int zOffs, byteArray blocks, BiomeArray biomes)
{
    for (int x = 0; x < 16; x++)
	{
        for (int z = 0; z < 16; z++)
		{
            int runDepth = 1;
            int run = -1;

            ::byte top = (::byte) Tile::whiteStone_Id;
            ::byte material = (::byte) Tile::whiteStone_Id;

            for (int y = Level::genDepthMinusOne; y >= 0; y--)
			{
                int offs = (z * 16 + x) * Level::genDepth + y;

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
                            material = (::byte) Tile::whiteStone_Id;
                        }

                        run = runDepth;
                        if (y >= 0) blocks[offs] = top;
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


doubleArray TheEndLevelRandomLevelSource::getHeights(doubleArray buffer, int x, int y, int z, int xSize, int ySize, int zSize)
{
    if (buffer.data == NULL)
	{
        buffer = doubleArray(xSize * ySize * zSize);
    }

    double s = 1 * 684.412;
    double hs = 1 * 684.412;

	doubleArray pnr, ar, br, sr, dr, fi, fis;	// 4J - used to be declared with class level scope but moved here for thread safety

    sr = scaleNoise->getRegion(sr, x, z, xSize, zSize, 1.121, 1.121, 0.5);
    dr = depthNoise->getRegion(dr, x, z, xSize, zSize, 200.0, 200.0, 0.5);

    s *= 2;

    pnr = perlinNoise1->getRegion(pnr, x, y, z, xSize, ySize, zSize, s / 80.0, hs / 160.0, s / 80.0);
    ar = lperlinNoise1->getRegion(ar, x, y, z, xSize, ySize, zSize, s, hs, s);
    br = lperlinNoise2->getRegion(br, x, y, z, xSize, ySize, zSize, s, hs, s);

    int p = 0;
    int pp = 0;

    for (int xx = 0; xx < xSize; xx++)
	{
        for (int zz = 0; zz < zSize; zz++)
		{
            double scale = ((sr[pp] + 256.0) / 512);
            if (scale > 1) scale = 1;


            double depth = (dr[pp] / 8000.0);
            if (depth < 0) depth = -depth * 0.3;
            depth = depth * 3.0 - 2.0;

            float xd = ((xx + x) - 0) / 1.0f;
            float zd = ((zz + z) - 0) / 1.0f;
            float doffs = 100 - sqrt(xd * xd + zd * zd) * 8;
            if (doffs > 80) doffs = 80;
            if (doffs < -100) doffs = -100;
            if (depth > 1) depth = 1;
            depth = depth / 8;
            depth = 0;

            if (scale < 0) scale = 0;
            scale = (scale) + 0.5;
            depth = depth * ySize / 16;

            pp++;

            double yCenter = ySize / 2.0;


            for (int yy = 0; yy < ySize; yy++)
			{
                double val = 0;
                double yOffs = (yy - (yCenter)) * 8 / scale;

                if (yOffs < 0) yOffs *= -1;

                double bb = ar[p] / 512;
                double cc = br[p] / 512;

                double v = (pnr[p] / 10 + 1) / 2;
                if (v < 0) val = bb;
                else if (v > 1) val = cc;
                else val = bb + (cc - bb) * v;
                val -= 8;
                val += doffs;

                int r = 2;
                if (yy > ySize / 2 - r)
				{
                    double slide = (yy - (ySize / 2 - r)) / (64.0f);
                    if (slide < 0) slide = 0;
                    if (slide > 1) slide = 1;
                    val = val * (1 - slide) + -3000 * slide;
                }
                r = 8;
                if (yy < r)
				{
                    double slide = (r - yy) / (r - 1.0f);
                    val = val * (1 - slide) + -30 * slide;
                }


                buffer[p] = val;
                p++;
            }
        }
    }








    return buffer;

}

