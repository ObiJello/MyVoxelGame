#include "LiquidFlow.h"
#include "Vec3.h"
#include "Mth.h"
#include <climits>
#include <cmath>
#include <stdexcept>
namespace console {
static int renderedDepth(const LiquidFlowAccess& access,int x,int y,int z){
    if (!access.sameLiquid(x,y,z)) return -1;
    int d = access.data(x,y,z);
    if(d<0 || d>15)throw std::out_of_range("Invalid liquid flow metadata");
    if (d >= 8) d = 0;
    return d;
}
static bool solidFace(const LiquidFlowAccess& access,int x,int y,int z,int face){
    
    if (access.sameLiquid(x,y,z)) return false;
	if (face == 1) return true;
    if (access.ice(x,y,z)) return false;
    
    return access.solidMaterial(x,y,z);
}
LiquidFlow consoleLiquidFlow(const LiquidFlowAccess& access,int x,int y,int z){for(int coordinate:{x,y,z})if(coordinate<=INT_MIN || coordinate>=INT_MAX)throw std::out_of_range("Liquid flow coordinate overflow");
    if(!access.sameLiquid(x,y,z))throw std::invalid_argument("Liquid flow requires a liquid source cell");

    ::Vec3 *flow = ::Vec3::newTemp(0,0,0);
    int mid = renderedDepth(access, x, y, z);
    for (int d = 0; d < 4; d++)
	{

        int xt = x;
        int yt = y;
        int zt = z;

        if (d == 0) xt--;
        if (d == 1) zt--;
        if (d == 2) xt++;
        if (d == 3) zt++;

        int t = renderedDepth(access, xt, yt, zt);
        if (t < 0)
		{
            if (!access.blocksMotion(xt,yt,zt))
			{
                t = renderedDepth(access, xt, yt - 1, zt);
                if (t >= 0)
				{
                    int dir = t - (mid - 8);
                    flow = flow->add((xt - x) * dir, (yt - y) * dir, (zt - z) * dir);
                }
            }
        } else
		{
            if (t >= 0)
			{
                int dir = t - mid;
                flow = flow->add((xt - x) * dir, (yt - y) * dir, (zt - z) * dir);
            }
        }

    }
    if (access.data(x,y,z) >= 8)
	{
        bool ok = false;
        if (ok || solidFace(access, x, y, z - 1, 2)) ok = true;
        if (ok || solidFace(access, x, y, z + 1, 3)) ok = true;
        if (ok || solidFace(access, x - 1, y, z, 4)) ok = true;
        if (ok || solidFace(access, x + 1, y, z, 5)) ok = true;
        if (ok || solidFace(access, x, y + 1, z - 1, 2)) ok = true;
        if (ok || solidFace(access, x, y + 1, z + 1, 3)) ok = true;
        if (ok || solidFace(access, x - 1, y + 1, z, 4)) ok = true;
        if (ok || solidFace(access, x + 1, y + 1, z, 5)) ok = true;
        if (ok) flow = flow->normalize()->add(0, -6, 0);
    }
    flow = flow->normalize();
    return {flow->x,flow->y,flow->z};
}
double consoleLiquidSlope(const LiquidFlowAccess& access,int x,int y,int z){auto flow=consoleLiquidFlow(access,x,y,z);
    if (flow.x == 0 && flow.z == 0) return -1000;
    return atan2(flow.z, flow.x) - PI / 2;
}
namespace {
struct LiquidIcon {
 float u0,v0,u1,v1;
 float getU0(bool adjust=false)const;float getU1(bool adjust=false)const;
 float getV0(bool adjust=false)const;float getV1(bool adjust=false)const;
 float getU(double offset,bool adjust=false)const;float getV(double offset,bool adjust=false)const;
};
static const float UVAdjust = (1.0f/16.0f)/256.0f;
float LiquidIcon::getU0(bool adjust)const{
	return adjust ? ( u0 + UVAdjust ) : u0;
}
float LiquidIcon::getU1(bool adjust)const{
	return adjust ? ( u1 - UVAdjust ) : u1;
}
float LiquidIcon::getV0(bool adjust)const{
	return adjust ? ( v0 + UVAdjust ) : v0;
}
float LiquidIcon::getV1(bool adjust)const{
	return adjust ? ( v1 - UVAdjust ) : v1;
}
float LiquidIcon::getU(double offset,bool adjust)const{
	float diff = getU1(adjust) - getU0(adjust);
	return getU0(adjust) + (diff * ((float) offset / 16));
}
float LiquidIcon::getV(double offset,bool adjust)const{
	float diff = getV1(adjust) - getV0(adjust);
	return getV0(adjust) + (diff * ((float) offset / 16));
}
constexpr LiquidIcon waterIcon{(1.0f/16.0f)*13,(1.0f/16.0f)*12,(1.0f/16.0f)*(13+1),(1.0f/16.0f)*(12+1)};
constexpr LiquidIcon water_flowIcon{(1.0f/16.0f)*14,(1.0f/16.0f)*12,(1.0f/16.0f)*(14+2),(1.0f/16.0f)*(12+2)};
constexpr LiquidIcon lavaIcon{(1.0f/16.0f)*13,(1.0f/16.0f)*14,(1.0f/16.0f)*(13+1),(1.0f/16.0f)*(14+1)};
constexpr LiquidIcon lava_flowIcon{(1.0f/16.0f)*14,(1.0f/16.0f)*14,(1.0f/16.0f)*(14+2),(1.0f/16.0f)*(14+2)};
}
std::array<LiquidUV,4> consoleLiquidTopUV(float angle,bool lava){
if(!std::isfinite(angle) || (angle!=-1000 && std::abs(angle)>7))throw std::invalid_argument("Invalid liquid slope angle");
auto* tex=angle < -999?(lava?&lavaIcon:&waterIcon):(lava?&lava_flowIcon:&water_flowIcon);
float u00, u01, u10, u11;
		float v00, v01, v10, v11;
		if ( angle < -999 )
		{
			u00 = tex->getU(0, true);
			v00 = tex->getV(0, true);
			u01 = u00;
			v01 = tex->getV(16, true);
			u10 = tex->getU(16, true);
			v10 = v01;
			u11 = u10;
			v11 = v00;
		}
		else
		{
			float s = Mth::sin(angle) * .25f;
			float c = Mth::cos(angle) * .25f;
			float cc = 16 * .5f;
			u00 = tex->getU(cc + (-c - s) * 16);
			v00 = tex->getV(cc + (-c + s) * 16);
			u01 = tex->getU(cc + (-c + s) * 16);
			v01 = tex->getV(cc + (+c + s) * 16);
			u10 = tex->getU(cc + (+c + s) * 16);
			v10 = tex->getV(cc + (+c - s) * 16);
			u11 = tex->getU(cc + (+c - s) * 16);
			v11 = tex->getV(cc + (-c - s) * 16);
		}

return {{{u00,v00},{u01,v01},{u10,v10},{u11,v11}}};
}
LiquidUV consoleLiquidSideUV(float height,bool right,bool lava){
if(!std::isfinite(height) || height < -.001f || height>1)throw std::invalid_argument("Invalid liquid side height");
const auto& tex=lava?lava_flowIcon:water_flowIcon;
return {tex.getU(right?8:0,true),height==0?tex.getV(8,true):tex.getV((1-height)*8)};
}
}
