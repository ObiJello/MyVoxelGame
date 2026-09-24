// Generated from Level::getSkyColor and the original colours.xml.
#include "SkyColour.h"
#include "ConsoleLightmap.h"
#include "Mth.h"
#include <cmath>
#include <stdexcept>
namespace console {
std::array<float,3> consoleSkyColour(int biome,std::int64_t time,float rain,float thunder,int lightningBoltTime,float a){
    if(biome<0 || biome>=23 || !std::isfinite(rain) || rain<0 || rain>1 || !std::isfinite(thunder) || thunder<0 || thunder>1 || lightningBoltTime<0)
        throw std::invalid_argument("Invalid sky colour input");
    static constexpr int colours[]={0x7BA5FF,0x79A7FF,0x6FB2FF,0x7EA3FF,0x7AA6FF,0x7FA2FF,0x79A7FF,0x7BA5FF,0x6FB2FF,0x000000,0x80A1FF,0x80A1FF,0x80A1FF,0x80A1FF,0x78A8FF,0x78A8FF,0x79A7FF,0x6FB2FF,0x7AA6FF,0x7FA2FF,0x7EA3FF,0x75ABFF,0x75ABFF};
    int skyColor=colours[biome];
    float td=consoleTimeOfDay(time,a);
    float br=Mth::cos(td * PI * 2) * 2 + 0.5f;
    if(br<0)br=0;
    if(br>1)br=1;
    float r = ((skyColor >> 16) & 0xff) / 255.0f;
	float g = ((skyColor >> 8) & 0xff) / 255.0f;
	float b = ((skyColor) & 0xff) / 255.0f;
	r *= br;
	g *= br;
	b *= br;

	float rainLevel = rain;
	if (rainLevel > 0)
	{
		float mid = (r * 0.30f + g * 0.59f + b * 0.11f) * 0.6f;

		float ba = 1 - rainLevel * 0.75f;
		r = r * ba + mid * (1 - ba);
		g = g * ba + mid * (1 - ba);
		b = b * ba + mid * (1 - ba);
	}
	float thunderLevel = thunder;
	if (thunderLevel > 0)
	{
		float mid = (r * 0.30f + g * 0.59f + b * 0.11f) * 0.2f;

		float ba = 1 - thunderLevel * 0.75f;
		r = r * ba + mid * (1 - ba);
		g = g * ba + mid * (1 - ba);
		b = b * ba + mid * (1 - ba);
	}

	if (lightningBoltTime > 0)
	{
		float f = (lightningBoltTime - a);
		if (f > 1) f = 1;
		f = f * 0.45f;
		r = r * (1 - f) + 0.8f * f;
		g = g * (1 - f) + 0.8f * f;
		b = b * (1 - f) + 1 * f;
	}

	return {r,g,b};
}
std::array<float,3> consoleCloudColour(std::int64_t time,float rain,float thunder,float a){
if(!std::isfinite(rain) || rain<0 || rain>1 || !std::isfinite(thunder) || thunder<0 || thunder>1)throw std::invalid_argument("Invalid cloud colour input");

	float td = consoleTimeOfDay(time,a);

	float br = Mth::cos(td * PI * 2) * 2.0f + 0.5f;
	if (br < 0.0f) br = 0.0f;
	if (br > 1.0f) br = 1.0f;

	int baseCloudColour = 0xFFFFFF;

	float r = ((baseCloudColour >> 16) & 0xff) / 255.0f;
	float g = ((baseCloudColour >> 8) & 0xff) / 255.0f;
	float b = ((baseCloudColour) & 0xff) / 255.0f;

	float rainLevel = rain;
	if (rainLevel > 0)
	{
		float mid = (r * 0.30f + g * 0.59f + b * 0.11f) * 0.6f;

		float ba = 1 - rainLevel * 0.95f;
		r = r * ba + mid * (1 - ba);
		g = g * ba + mid * (1 - ba);
		b = b * ba + mid * (1 - ba);
	}

	r *= br * 0.90f + 0.10f;
	g *= br * 0.90f + 0.10f;
	b *= br * 0.85f + 0.15f;

	float thunderLevel = thunder;
	if (thunderLevel > 0)
	{
		float mid = (r * 0.30f + g * 0.59f + b * 0.11f) * 0.2f;

		float ba = 1 - thunderLevel * 0.95f;
		r = r * ba + mid * (1 - ba);
		g = g * ba + mid * (1 - ba);
		b = b * ba + mid * (1 - ba);
	}

	return {r,g,b};
}
}
