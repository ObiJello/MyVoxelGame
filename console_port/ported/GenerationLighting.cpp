// Extracted original CPU lighting; cache disabled and lock made exception-safe.
#include "WorldGenLevel.h"

void Level::checkLight(int x, int y, int z, bool force, bool rootOnlyEmissive)		// 4J added force, rootOnlyEmissive parameters
{
    if (!dimension->hasCeiling) checkLight(LightLayer::Sky, x, y, z, force, false);
    checkLight(LightLayer::Block, x, y, z, force, rootOnlyEmissive);
}

int Level::getExpectedSkyColor(lightCache_t *cache, int oc, int x, int y , int z, int ct, int block)
{
    int expected = 0;

	if( block == 255 ) return 0;		// 4J added as optimisation

    if (canSeeSky(x, y, z))
	{
        expected = 15;
    }
	else
	{
        if (block == 0) block = 1;

		// 4J - changed this to attempt to get all 6 brightnesses of neighbours in a single call, as an optimisation
		int b[6];
		b[0] = getBrightnessCached(cache, LightLayer::Sky, x - 1, y, z);
		b[1] = getBrightnessCached(cache, LightLayer::Sky, x + 1, y, z);
		b[2] = getBrightnessCached(cache, LightLayer::Sky, x, y - 1, z);
		b[3] = getBrightnessCached(cache, LightLayer::Sky, x, y + 1, z);
		b[4] = getBrightnessCached(cache, LightLayer::Sky, x, y, z - 1);
		b[5] = getBrightnessCached(cache, LightLayer::Sky, x, y, z + 1);
		for( int i = 0; i < 6; i++ )
		{
			if( ( b[i] - block ) > expected ) expected = b[i] - block;
		}
    }

    return expected;
}

int Level::getExpectedBlockColor(lightCache_t *cache, int oc, int x, int y, int z, int ct, int block, bool propagatedOnly)
{
    int expected = propagatedOnly ? 0 : getEmissionCached(cache, ct, x, y, z);

	if( block >= 15 ) return expected;	// 4J added as optimisation

	// 4J - changed this to attempt to get all 6 brightnesses of neighbours in a single call, as an optimisation
	int b[6];
	b[0] = getBrightnessCached(cache, LightLayer::Block, x - 1, y, z);
	b[1] = getBrightnessCached(cache, LightLayer::Block, x + 1, y, z);
	b[2] = getBrightnessCached(cache, LightLayer::Block, x, y - 1, z);
	b[3] = getBrightnessCached(cache, LightLayer::Block, x, y + 1, z);
	b[4] = getBrightnessCached(cache, LightLayer::Block, x, y, z - 1);
	b[5] = getBrightnessCached(cache, LightLayer::Block, x, y, z + 1);
	for( int i = 0; i < 6; i++ )
	{
		if( ( b[i] - block ) > expected ) expected = b[i] - block;
	}

    return expected;
}

void Level::checkLight(LightLayer::variety layer, int xc, int yc, int zc, bool force, bool rootOnlyEmissive)
{
	lightCache_t *cache = nullptr;
	__uint64 cacheUse = 0;

	if( force )
	{
		// 4J - special mode added so we can do lava lighting updates without having all neighbouring chunks loaded in
		if (!hasChunksAt(xc, yc, zc, 0)) return;
	}
	else
	{
		// 4J - this is normal java behaviour
	    if (!hasChunksAt(xc, yc, zc, 17)) return;
	}

#if 0
	/////////////////////////////////////////////////////////////////////////////////////////////
	 // Get the frequency of the timer
	LARGE_INTEGER qwTicksPerSec, qwTime, qwNewTime, qwDeltaTime1, qwDeltaTime2;
	float fElapsedTime1 = 0.0f;
	float fElapsedTime2 = 0.0f;
	QueryPerformanceFrequency( &qwTicksPerSec );
	float fSecsPerTick = 1.0f / (float)qwTicksPerSec.QuadPart;

	QueryPerformanceCounter( &qwTime );
	/////////////////////////////////////////////////////////////////////////////////////////////
#endif

	std::lock_guard<std::mutex> guard(m_checkLightCS);

#ifdef __PSVITA__
	// AP - only clear the one array element required to check if something has changed 
	cachewritten = false;
	if( cache != NULL )
	{
		int idx;
		if( !(yc & 0xffffff00) )
		{
			idx = GetIndex(xc, yc, zc);
			cache[idx] = 0;
			idx = GetIndex(xc - 1, yc, zc);
			cache[idx] = 0;
			idx = GetIndex(xc + 1, yc, zc);
			cache[idx] = 0;
			idx = GetIndex(xc, yc, zc - 1);
			cache[idx] = 0;
			idx = GetIndex(xc, yc, zc + 1);
			cache[idx] = 0;
		}
		if( !((yc-1) & 0xffffff00) )
		{
			idx = GetIndex(xc, yc - 1, zc);
			cache[idx] = 0;
		}
		if( !((yc+1) & 0xffffff00) )
		{
			idx = GetIndex(xc, yc + 1, zc);
			cache[idx] = 0;
		}
	}
#else
	initCache(cache);
#endif

	// If we're in cached mode, then use memory allocated after the cached data itself for the toCheck array, in an attempt to make both that & the other cached data sit on the CPU L2 cache better.
	
	int *toCheck;
	if( cache == NULL )
	{
		toCheck = toCheckLevel;
	}
	else
	{
		toCheck = (int *)(cache + (16*16*16));
	}

    int tcp = 0;
    int tcc = 0;
	//int darktcc = 0;


	// 4J - added
	int minXZ = - (dimension->getXZSize() * 16 ) / 2;
	int maxXZ = (dimension->getXZSize() * 16 ) / 2 - 1;
	if( ( xc > maxXZ ) || ( xc < minXZ ) || ( zc > maxXZ ) || ( zc < minXZ ) )
	{
		
		return;
	}

	// Lock 128K of cache (containing all the lighting cache + first 112K of toCheck array) on L2 to try and stop any cached data getting knocked out of L2 by other non-cached reads (or vice-versa)
//	if( cache ) XLockL2(XLOCKL2_INDEX_TITLE, cache, 128 * 1024, XLOCKL2_LOCK_SIZE_1_WAY, 0 );
	
    {
        int cc = getBrightnessCached(cache, layer, xc, yc, zc);
        int ex = 0;
        {
			int ct = 0;
			int block = getBlockingCached(cache, layer, &ct, xc, yc, zc);
            if (block == 0) block = 1;

            int expected = 0;
            if (layer == LightLayer::Sky)
			{
                expected = getExpectedSkyColor(cache, cc, xc, yc, zc, ct, block);
            }
			else
			{
                expected = getExpectedBlockColor(cache, cc, xc, yc, zc, ct, block, false);
            }

            ex = expected;

        }

#ifdef __PSVITA__
		// AP - we only need to memset the entire array if we discover something has changed
		if( ex != cc && cache )
		{
			lightCache_t old[7];
			if( !(yc & 0xffffff00) )
			{
				old[0] = cache[GetIndex(xc, yc, zc)];
				old[1] = cache[GetIndex(xc - 1, yc, zc)];
				old[2] = cache[GetIndex(xc + 1, yc, zc)];
				old[5] = cache[GetIndex(xc, yc, zc - 1)];
				old[6] = cache[GetIndex(xc, yc, zc + 1)];
			}
			if( !((yc-1) & 0xffffff00) )
			{
				old[3] = cache[GetIndex(xc, yc - 1, zc)];
			}
			if( !((yc+1) & 0xffffff00) )
			{
				old[4] = cache[GetIndex(xc, yc + 1, zc)];
			}

			XMemSet128(cache,0,16*16*16*sizeof(lightCache_t));

			if( !(yc & 0xffffff00) )
			{
				cache[GetIndex(xc, yc, zc)] = old[0];
				cache[GetIndex(xc - 1, yc, zc)] = old[1];
				cache[GetIndex(xc + 1, yc, zc)] = old[2];
				cache[GetIndex(xc, yc, zc - 1)] = old[5];
				cache[GetIndex(xc, yc, zc + 1)] = old[6];
			}
			if( !((yc-1) & 0xffffff00) )
			{
				cache[GetIndex(xc, yc - 1, zc)] = old[3];
			}
			if( !((yc+1) & 0xffffff00) )
			{
				cache[GetIndex(xc, yc + 1, zc)] = old[4];
			}
		}
#endif

        if (ex > cc)
		{
            toCheck[tcc++] = ((32)) + ((32) << 6) + ((32) << 12);
        }
		else if (ex < cc)
		{
			// 4J - added tcn. This is the code that is run when checkLight has been called for a light source that has got darker / turned off.
			// In the original version, after zeroing tiles brightnesses that are deemed to come from this light source, all the zeroed tiles are then passed to the next
			// stage of the function to potentially have their brightnesses put back up again. We shouldn't need to consider All these tiles as starting points for this process, now just
			// considering the edge tiles (defined as a tile where we have a neighbour that is brightner than can be explained by the original light source we are turning off)
			int tcn = 0;
            if (layer == LightLayer::Block || true)
			{
                toCheck[tcc++] = ((32)) + ((32) << 6) + ((32) << 12) + (cc << 18);
                while (tcp < tcc)
				{
                    int p = toCheck[tcp++];
                    int x = ((p) & 63) - 32 + xc;
                    int y = ((p >> 6) & 63) - 32 + yc;
                    int z = ((p >> 12) & 63) - 32 + zc;
                    int cexp = ((p >> 18) & 15);
                    int o = getBrightnessCached(cache, layer, x, y, z);
                    if (o == cexp)
					{
                        setBrightnessCached(cache, &cacheUse, layer, x, y, z, 0);
                        // cexp--;		// 4J - removed, change from 1.2.3
                        if (cexp > 0)
						{
                            int xd = x - xc;
                            int yd = y - yc;
                            int zd = z - zc;
                            if (xd < 0) xd = -xd;
                            if (yd < 0) yd = -yd;
                            if (zd < 0) zd = -zd;
                            if (xd + yd + zd < 17)
							{
								bool edge = false;
                                for (int j = 0; j < 6; j++)
								{
                                    int flip = j % 2 * 2 - 1;

                                    int xx = x + ((j / 2) % 3 / 2) * flip;
                                    int yy = y + ((j / 2 + 1) % 3 / 2) * flip;
                                    int zz = z + ((j / 2 + 2) % 3 / 2) * flip;

									// 4J - added - don't let this lighting creep out of the normal fixed world and into the infinite water chunks beyond
									if( ( xx > maxXZ ) || ( xx < minXZ ) || ( zz > maxXZ ) || ( zz < minXZ ) ) continue;
									if( ( yy < 0 ) || ( yy >= maxBuildHeight ) ) continue;

                                    o = getBrightnessCached(cache, layer, xx, yy, zz);
									// 4J - some changes here brought forward from 1.2.3
									int block = getBlockingCached(cache, layer, NULL, xx, yy, zz);
                                    if (block == 0) block = 1;
                                    if ((o == cexp - block) && (tcc < (32 * 32 * 32))) // 4J - 32 * 32 * 32 was toCheck.length
									{
                                        toCheck[tcc++] = (((xx - xc) + 32)) + (((yy - yc) + 32) << 6) + (((zz - zc) + 32) << 12) + ((cexp - block) << 18);
									}
									else
									{
										// 4J - added - keep track of which tiles form the edge of the region we are zeroing
										if( o > ( cexp - block ) )
										{
											edge = true;
										}
									}
                                }
								// 4J - added - keep track of which tiles form the edge of the region we are zeroing - can store over the original elements in the array because tcn must be <= tcp
								if( edge == true )
								{
									toCheck[tcn++] = p;
								}
                            }
                        }

                    }
                }
            }
			tcp = 0;
//			darktcc = tcc;	///////////////////////////////////////////////////
			tcc = tcn;	// 4J added - we've moved all the edge tiles to the start of the array, so only need to process these now. The original processes all tcc tiles again in the next section
        }
    }

    while (tcp < tcc)
	{
        int p = toCheck[tcp++];
        int x = ((p) & 63) - 32 + xc;
        int y = ((p >> 6) & 63) - 32 + yc;
        int z = ((p >> 12) & 63) - 32 + zc;

		// If force is set, then this is being used to in a special mode to try and light lava tiles as chunks are being loaded in. In this case, we
		// don't want a lighting update to drag in any neighbouring chunks that aren't loaded yet.
		if( force )
		{
			if( !hasChunkAt(x,y,z) )
			{
				continue;
			}
		}

        int c = getBrightnessCached(cache, layer, x, y, z);
		int ct = 0;
		int block = getBlockingCached(cache, layer, &ct, x, y, z);
        if (block == 0) block = 1;

        int expected = 0;
        if (layer == LightLayer::Sky)
		{
            expected = getExpectedSkyColor(cache, c, x, y, z, ct, block);
        }
		else
		{
			// If rootOnlyEmissive flag is set, then only consider the starting tile to be possibly emissive.
			bool propagatedOnly = false;
			if( rootOnlyEmissive )
			{
				propagatedOnly = ( x != xc ) || ( y != yc ) || ( z != zc );
			}
            expected = getExpectedBlockColor(cache, c, x, y, z, ct, block, propagatedOnly);
        }

        if (expected != c)
		{
            setBrightnessCached(cache, &cacheUse, layer, x, y, z, expected);

            if (expected > c)
			{
                int xd = x - xc;
                int yd = y - yc;
                int zd = z - zc;
                if (xd < 0) xd = -xd;
                if (yd < 0) yd = -yd;
                if (zd < 0) zd = -zd;
                if (xd + yd + zd < 17 && tcc < (32 * 32 * 32) - 6)		// 4J - 32 * 32 * 32 was toCheck.length
				{
					// 4J - added extra checks here to stop lighting updates moving out of the actual fixed world and into the infinite water chunks
					if( ( x - 1 ) >= minXZ ) { if (getBrightnessCached(cache, layer, x - 1, y, z) < expected) toCheck[tcc++] = (((x - 1 - xc) + 32)) + (((y - yc) + 32) << 6) + (((z - zc) + 32) << 12); }
					if( ( x + 1 ) <= maxXZ ) { if (getBrightnessCached(cache, layer, x + 1, y, z) < expected) toCheck[tcc++] = (((x + 1 - xc) + 32)) + (((y - yc) + 32) << 6) + (((z - zc) + 32) << 12); }
					if( ( y - 1 ) >= 0 )     { if (getBrightnessCached(cache, layer, x, y - 1, z) < expected) toCheck[tcc++] = (((x - xc) + 32)) + (((y - 1 - yc) + 32) << 6) + (((z - zc) + 32) << 12); }
					if( ( y + 1 ) < maxBuildHeight ) { if (getBrightnessCached(cache, layer, x, y + 1, z) < expected) toCheck[tcc++] = (((x - xc) + 32)) + (((y + 1 - yc) + 32) << 6) + (((z - zc) + 32) << 12); }
					if( ( z - 1 ) >= minXZ ) { if (getBrightnessCached(cache, layer, x, y, z - 1) < expected) toCheck[tcc++] = (((x - xc) + 32)) + (((y - yc) + 32) << 6) + (((z - 1 - zc) + 32) << 12); }
					if( ( z + 1 ) <= maxXZ ) { if (getBrightnessCached(cache, layer, x, y, z + 1) < expected) toCheck[tcc++] = (((x - xc) + 32)) + (((y - yc) + 32) << 6) + (((z + 1 - zc) + 32) << 12); }
                }
            }
        }
    }
//	if( cache ) XUnlockL2(XLOCKL2_INDEX_TITLE);
#if 0
	QueryPerformanceCounter( &qwNewTime );
	qwDeltaTime1.QuadPart = qwNewTime.QuadPart - qwTime.QuadPart;
	qwTime = qwNewTime;
#endif

	flushCache(cache, cacheUse, layer);
#if 0
	/////////////////////////////////////////////////////////////////
	if( cache )
	{
		QueryPerformanceCounter( &qwNewTime );
		qwDeltaTime2.QuadPart = qwNewTime.QuadPart - qwTime.QuadPart;
		fElapsedTime1 = fSecsPerTick * ((FLOAT)(qwDeltaTime1.QuadPart));
		fElapsedTime2 = fSecsPerTick * ((FLOAT)(qwDeltaTime2.QuadPart));
		if( ( darktcc > 0 ) | ( tcc > 0 ) )
		{
			printf("%d %d %d %f + %f = %f\n", darktcc, tcc, darktcc + tcc, fElapsedTime1 * 1000.0f, fElapsedTime2 * 1000.0f, ( fElapsedTime1 + fElapsedTime2 ) * 1000.0f);
		}
	}
	/////////////////////////////////////////////////////////////////
#endif
	
	
}
