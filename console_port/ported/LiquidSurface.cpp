#include "LiquidSurface.h"
#include <climits>
#include <stdexcept>
namespace console {
float consoleLiquidDepth(int d) {
if(d<0 || d>15)throw std::out_of_range("Invalid liquid data nibble");

    if (d >= 8) d = 0;
    return (d + 1) / 9.0f;
}
float consoleLiquidCorner(const LiquidSurfaceAccess& access,int x,int y,int z) {
for(int coordinate:{x,y,z})if(coordinate<=INT_MIN || coordinate>=INT_MAX)throw std::out_of_range("Liquid corner coordinate overflow");

	int		count = 0;
	float	h = 0;
	for ( int i = 0; i < 4; i++ )
	{
		int			xx = x - ( i & 1 );
		int			yy = y;
		int			zz = z - ( ( i >> 1 ) & 1 );
		if ( access.sameLiquid(xx,yy+1,zz) )
		{
			return 1;
		}
		bool same = access.sameLiquid(xx,yy,zz);
		if ( same )
		{
			int d = access.data(xx,yy,zz);
			if ( d >= 8 || d == 0 )
			{
				h += ( consoleLiquidDepth( d ) )* 10;
				count += 10;
			}
			h += consoleLiquidDepth( d );
			count++;
		}
		else if ( !access.solidMaterial(xx,yy,zz) )
		{
			h += 1;
			count++;
		}
	}
	if(count==0)throw std::invalid_argument("Liquid corner has no liquid or open samples");
	return 1 - h / count;
}
int sampleConsoleLiquidLight(const RenderLightAccess& access,int x,int y,int z,int tileId) {
if(y>=INT_MAX-1)throw std::out_of_range("Liquid light sample height overflow");

	// 4J - note that this code seems to basically be a hack to fix a problem where post-processed things like lakes aren't getting lit properly
    int a = sampleConsoleLight(access,x, y, z, 0, tileId);
    int b = sampleConsoleLight(access,x, y + 1, z, 0, tileId);

    int aa = a & 0xff;
    int ba = b & 0xff;
    int ab = (a >> 16) & 0xff;
    int bb = (b >> 16) & 0xff;

    return (aa > ba ? aa : ba) | ((ab > bb ? ab : bb) << 16);
}
}
