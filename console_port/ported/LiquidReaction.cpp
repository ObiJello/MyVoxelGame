// Generated from original LiquidTile::updateLiquid. Effects are returned to the host.
#include "LiquidReaction.h"
#include <climits>
#include <stdexcept>
namespace console {
LiquidReaction consoleLiquidReaction(const LiquidReactionAccess& access,int x,int y,int z){
    if(x<=INT_MIN || x>=INT_MAX || y>=INT_MAX || z<=INT_MIN || z>=INT_MAX)
        throw std::out_of_range("Liquid reaction coordinate overflow");
    LiquidReaction result;

    if (!access.lava(x,y,z)) return result;
    if (access.lava(x,y,z))
	{
        bool water = false;
        if (water || access.water(x, y, z - 1)) water = true;
        if (water || access.water(x, y, z + 1)) water = true;
        if (water || access.water(x - 1, y, z)) water = true;
        if (water || access.water(x + 1, y, z)) water = true;
        if (water || access.water(x, y + 1, z)) water = true;
        if (water)
		{
            int data = access.data(x,y,z);
            if(data<0 || data>15)throw std::out_of_range("Invalid liquid reaction metadata");
            if (data == 0)
			{
                result.replacement=49;
            }
			else if (data <= 4)
			{
                result.replacement=4;
            }
            result.fizz=true;
        }
    }


return result;
}
}
