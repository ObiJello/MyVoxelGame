#include "stdafx.h"
#include "net.minecraft.world.level.h"
#include "LargeFeature.h"

const wstring LargeFeature::STRONGHOLD = L"StrongHold";

LargeFeature::LargeFeature()
{
	radius = 8;
	random = new Random();
}

LargeFeature::~LargeFeature()
{
	delete random;
}

void LargeFeature::apply(ChunkSource *ChunkSource, Level *level, int xOffs, int zOffs, byteArray blocks)
{
    int r = radius;
	this->level = level;

    random->setSeed(level->getSeed());
    __int64 xScale = random->nextLong();
    __int64 zScale = random->nextLong();

    for (int x = xOffs - r; x <= xOffs + r; x++)
	{
        for (int z = zOffs - r; z <= zOffs + r; z++)
		{
            std::uint64_t xx = static_cast<std::uint64_t>(x) * static_cast<std::uint64_t>(xScale);
            std::uint64_t zz = static_cast<std::uint64_t>(z) * static_cast<std::uint64_t>(zScale);
            random->setSeed(xx ^ zz ^ level->getSeed());
            addFeature(level, x, z, xOffs, zOffs, blocks);
        }
    }
}