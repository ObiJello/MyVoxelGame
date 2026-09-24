// Generated from GrassTile::getColor; LeafTile/LiquidTile use the same average.
#include "BiomeTint.h"
#include <stdexcept>
namespace console {
int consoleBiomeTint(TintKind kind,const std::array<int,9>& biomes,int leafData){
 if(static_cast<int>(kind)<0 || static_cast<int>(kind)>2 || leafData<0 || leafData>15)throw std::invalid_argument("Invalid biome tint input");
 for(int id:biomes)if(id<0 || id>=23)throw std::invalid_argument("Invalid tint biome");
 if(kind==TintKind::Foliage && (leafData&3)==1)return 0x619961;
 if(kind==TintKind::Foliage && (leafData&3)==2)return 0x80a755;
 static constexpr int colours[3][23]={
{0x8eb971,0x91bd59,0xbfb755,0x8ab689,0x79c05a,0x82b593,0x5c694e,0x8eb971,0xbfb755,0x8eb971,0x80b497,0x80b497,0x80b497,0x80b497,0x55c93f,0x55c93f,0x91bd59,0xbfb755,0x79c05a,0x82b593,0x8ab689,0x53ca37,0x53ca37},
{0x71a74d,0x77ab2f,0xaea42a,0x6da36b,0x59ae30,0x63a277,0x496137,0x71a74d,0xaea42a,0x71a74d,0x60a17b,0x60a17b,0x60a17b,0x60a17b,0x2bbb0f,0x2bbb0f,0x77ab2f,0xaea42a,0x59ae30,0x63a277,0x6da36b,0x29bc05,0x29bc05},
{0xffffff,0xffffff,0xffffff,0xffffff,0xffffff,0xffffff,0xe0ffae,0xffffff,0xffffff,0xffffff,0xffffff,0xffffff,0xffffff,0xffffff,0xffffff,0xffffff,0xffffff,0xffffff,0xffffff,0xffffff,0xffffff,0xffffff,0xffffff},
 };
int totalRed = 0;
	int totalGreen = 0;
	int totalBlue = 0;

	for (int oz = -1; oz <= 1; oz++)
	{
		for (int ox = -1; ox <= 1; ox++)
		{
			int grassColor = colours[static_cast<int>(kind)][biomes[(oz+1)*3+ox+1]];

			totalRed += (grassColor & 0xff0000) >> 16;
			totalGreen += (grassColor & 0xff00) >> 8;
			totalBlue += (grassColor & 0xff);
		}
	}

	return (((totalRed / 9) & 0xFF) << 16) | (((totalGreen / 9) & 0xFF) << 8) | (((totalBlue / 9) & 0xFF));
}
}
