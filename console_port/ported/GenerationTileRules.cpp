// Original placement methods extracted by tools/extract_tile_rules.py.
#include "WorldGenLevel.h"
bool Tile::mayPlace(Level *level, int x, int y, int z, int face)
{
	return mayPlace(level, x, y, z);
}

bool Tile::mayPlace(Level *level, int x, int y, int z)
{
	int t = level->getTile(x, y, z);
	return t == 0 || Tile::tiles[t]->material->isReplaceable();
}

bool Bush::mayPlace(Level *level, int x, int y, int z)
{
	return Tile::mayPlace(level, x, y, z) && mayPlaceOn(level->getTile(x, y - 1, z));
}

bool Bush::mayPlaceOn(int tile)
{
	return tile == Tile::grass_Id || tile == Tile::dirt_Id || tile == Tile::farmland_Id;
}

bool Bush::canSurvive(Level *level, int x, int y, int z)
{
	return ( level->getDaytimeRawBrightness(x, y, z) >= 8 || (level->canSeeSky(x, y, z))) && mayPlaceOn(level->getTile(x, y - 1, z));
}

bool DeadBushTile::mayPlaceOn(int tile)
{
	return tile == Tile::sand_Id;
}

bool Mushroom::mayPlace(Level *level, int x, int y, int z)
{
	return Bush::mayPlace(level, x, y, z) && canSurvive(level, x, y, z);
}

bool Mushroom::mayPlaceOn(int tile)
{
	return Tile::solid[tile];
}

bool Mushroom::canSurvive(Level *level, int x, int y, int z)
{
	if (y < 0 || y >= Level::maxBuildHeight) return false;

    int below = level->getTile(x, y - 1, z);

    return below == Tile::mycel_Id || (level->getDaytimeRawBrightness(x, y, z) < 13 && mayPlaceOn(below));
}

bool CactusTile::mayPlace(Level *level, int x, int y, int z)
{
    if (!Tile::mayPlace(level, x, y, z)) return false;

    return canSurvive(level, x, y, z);
}

bool CactusTile::canSurvive(Level *level, int x, int y, int z)
{
    if (level->getMaterial(x - 1, y, z)->isSolid()) return false;
    if (level->getMaterial(x + 1, y, z)->isSolid()) return false;
    if (level->getMaterial(x, y, z - 1)->isSolid()) return false;
    if (level->getMaterial(x, y, z + 1)->isSolid()) return false;
    int below = level->getTile(x, y - 1, z);
    return below == Tile::cactus_Id || below == Tile::sand_Id;
}

bool ReedTile::mayPlace(Level *level, int x, int y, int z) 
{
	int below = level->getTile(x, y - 1, z);
	if (below == id) return true;
	if (below != Tile::grass_Id && below != Tile::dirt_Id && below != Tile::sand_Id) return false;
	if (level->getMaterial(x - 1, y - 1, z) == Material::water) return true;
	if (level->getMaterial(x + 1, y - 1, z) == Material::water) return true;
	if (level->getMaterial(x, y - 1, z - 1) == Material::water) return true;
	if (level->getMaterial(x, y - 1, z + 1) == Material::water) return true;
	//printf("no water\n");
	return false;
}

bool ReedTile::canSurvive(Level *level, int x, int y, int z)
{
	return mayPlace(level, x, y, z);
}

bool WaterlilyTile::mayPlaceOn(int tile)
{
	return tile == Tile::calmWater_Id;
}

bool WaterlilyTile::canSurvive(Level *level, int x, int y, int z)
{
	if (y < 0 || y >= Level::maxBuildHeight) return false;
    return level->getMaterial(x, y - 1, z) == Material::water && level->getData(x, y - 1, z) == 0;
}

bool PumpkinTile::mayPlace(Level *level, int x, int y, int z)
{
    int t = level->getTile(x, y, z);
    return (t == 0 || Tile::tiles[t]->material->isReplaceable()) && level->isTopSolidBlocking(x, y - 1, z);

}

bool VineTile::mayPlace(Level *level, int x, int y, int z, int face)
{
    switch (face)
	{
		default:
			return false;
		case Facing::UP:
			return isAcceptableNeighbor(level->getTile(x, y + 1, z));
		case Facing::NORTH:
			return isAcceptableNeighbor(level->getTile(x, y, z + 1));
		case Facing::SOUTH:
			return isAcceptableNeighbor(level->getTile(x, y, z - 1));
		case Facing::EAST:
			return isAcceptableNeighbor(level->getTile(x - 1, y, z));
		case Facing::WEST:
			return isAcceptableNeighbor(level->getTile(x + 1, y, z));
    }
}

bool VineTile::isAcceptableNeighbor(int id)
{
    if (id == 0) return false;
    Tile *tile = Tile::tiles[id];
    if (tile->isCubeShaped() && tile->material->blocksMotion()) return true;
    return false;
}

bool Level::isTopSolidBlocking(int x, int y, int z)
{
    // Temporary workaround until tahgs per-face solidity is finished
    Tile *tile = Tile::tiles[getTile(x, y, z)];
    if (tile == NULL) return false;

    if (tile->material->isSolidBlocking() && tile->isCubeShaped()) return true;
	if (dynamic_cast<StairTile *>(tile) != NULL) 
	{
		return (getData(x, y, z) & StairTile::UPSIDEDOWN_BIT) == StairTile::UPSIDEDOWN_BIT;
	}
    if (dynamic_cast<HalfSlabTile *>(tile) != NULL)
	{
		return (getData(x, y, z) & HalfSlabTile::TOP_SLOT_BIT) == HalfSlabTile::TOP_SLOT_BIT;
	}
	if (dynamic_cast<TopSnowTile *>(tile) != NULL) return (getData(x, y, z) & TopSnowTile::HEIGHT_MASK) == TopSnowTile::MAX_HEIGHT + 1;
    return false;
}

bool Level::shouldFreezeIgnoreNeighbors(int x, int y, int z)
{
    return shouldFreeze(x, y, z, false);
}

bool Level::shouldFreeze(int x, int y, int z)
{
    return shouldFreeze(x, y, z, true);
}

bool Level::shouldFreeze(int x, int y, int z, bool checkNeighbors)
{
    Biome *biome = getBiome(x, z);
    float temp = biome->getTemperature();
    if (temp > 0.15f) return false;

    if (y >= 0 && y < maxBuildHeight && getBrightness(LightLayer::Block, x, y, z) < 10)
	{
        int current = getTile(x, y, z);
        if ((current == Tile::calmWater_Id || current == Tile::water_Id) && getData(x, y, z) == 0)
		{
            if (!checkNeighbors) return true;

            bool surroundedByWater = true;
            if (surroundedByWater && getMaterial(x - 1, y, z) != Material::water) surroundedByWater = false;
            if (surroundedByWater && getMaterial(x + 1, y, z) != Material::water) surroundedByWater = false;
            if (surroundedByWater && getMaterial(x, y, z - 1) != Material::water) surroundedByWater = false;
            if (surroundedByWater && getMaterial(x, y, z + 1) != Material::water) surroundedByWater = false;
            if (!surroundedByWater) return true;
        }
    }
    return false;
}

