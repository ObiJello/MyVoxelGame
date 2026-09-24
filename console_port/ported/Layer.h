#pragma once

#include "ArrayWithLength.h"

class LevelType;

#ifndef _CONTENT_PACAKGE
#define _BIOME_OVERRIDE
#endif

class Layer
{
private:
	std::uint64_t seed;

protected:
	shared_ptr<Layer>parent;

private:
	std::uint64_t rval;
	std::uint64_t seedMixup;

public:
	static LayerArray getDefaultLayers(__int64 seed, LevelType *levelType);

	Layer(__int64 seedMixup);

	virtual void init(__int64 seed);
	virtual void initRandom(__int64 x, __int64 y);

protected:
	int nextRandom(int max);

public:
	virtual intArray getArea(int xo, int yo, int w, int h) = 0;
};