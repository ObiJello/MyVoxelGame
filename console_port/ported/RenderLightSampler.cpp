#include "RenderLightAccess.h"
#include <climits>
#include <stdexcept>
namespace console {
static int propagated(const RenderLightAccess& access,LightLayer::variety layer,int x,int y,int z,int tileId)
{
	if (access.hasCeiling() && layer == LightLayer::Sky) return 0;

    if (y < 0) y = 0;
    if (y >= 256 && layer == LightLayer::Sky)
	{
		// 4J Stu - The java LightLayer was an enum class type with a member "surrounding" which is what we
		// were returning here. Surrounding has the same value as the enum value in our C++ code, so just cast
		// it to an int
		return (int)layer;
    }
    if (x < -30000000 || z < -30000000 || x >= 30000000 || z >= 30000000)
	{
		// 4J Stu - The java LightLayer was an enum class type with a member "surrounding" which is what we
		// were returning here. Surrounding has the same value as the enum value in our C++ code, so just cast
		// it to an int
		return (int)layer;
    }
    int xc = x >> 4;
    int zc = z >> 4;
    if (!access.hasChunk(xc, zc)) return (int)layer;

    {
		int id = tileId > -1 ? tileId : access.tile(x,y,z);
		if (access.propagates(id))
		{
            int br = access.brightness(layer, x, y + 1, z);
            int br1 = access.brightness(layer, x + 1, y, z);
            int br2 = access.brightness(layer, x - 1, y, z);
            int br3 = access.brightness(layer, x, y, z + 1);
            int br4 = access.brightness(layer, x, y, z - 1);
            if (br1 > br) br = br1;
            if (br2 > br) br = br2;
            if (br3 > br) br = br3;
            if (br4 > br) br = br4;
            return br;
        }
    }

    return access.storedLight(layer,x,y,z);
}
int sampleConsoleLight(const RenderLightAccess& access,int x,int y,int z,int emitt,int tileId)
{
    for(int coordinate:{x,y,z})if(coordinate <= INT_MIN || coordinate >= INT_MAX)throw std::out_of_range("Render light coordinate overflow");
    if(emitt<0 || emitt>15 || tileId < -1 || tileId > 254)throw std::out_of_range("Invalid render light tile/emission");
	int s = propagated(access,LightLayer::Sky, x, y, z, tileId);
    int b = propagated(access,LightLayer::Block, x, y, z, tileId);
    if (b < emitt) b = emitt;
    return s << 20 | b << 4;
}

}
