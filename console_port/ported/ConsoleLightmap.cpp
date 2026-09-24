#include "ConsoleLightmap.h"
#include "Mth.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace console {
namespace {
void fraction(float value) { if (!std::isfinite(value) || value < 0 || value > 1) throw std::invalid_argument("Invalid lightmap fraction"); }
void validDimension(int value) { if (value < -1 || value > 1) throw std::invalid_argument("Invalid lightmap dimension"); }
}
float consoleTimeOfDay(std::int64_t time,float a,int dimension) {
fraction(a);validDimension(dimension);
if(dimension == -1)return 0.5f; if(dimension == 1)return 0.0f;

    int dayStep = (int) (time % 24000);
    float td = (dayStep + a) / 24000 - 0.25f;
    if (td < 0) td += 1;
    if (td > 1) td -= 1;
    float tdo = td;
    td = 1 - (float) ((cos(td * PI) + 1) / 2);
    td = tdo + (td - tdo) / 3.0f;
    return td;
}
float consoleSkyDarken(std::int64_t time,float rain,float thunder,float a,int dimension) {
fraction(rain);fraction(thunder);

    float td = consoleTimeOfDay(time,a,dimension);

    float br = 1 - (Mth::cos(td * PI * 2) * 2 + 0.2f);
    if (br < 0.0f) br = 0.0f;
    if (br > 1.0f) br = 1.0f;

    br = 1.0f - br;

    br *= 1.0f - (rain * 5.0f / 16.0f);
    br *= 1.0f - (thunder * 5.0f / 16.0f);
    // return ((int) (br * 13));

    return br * 0.8f + 0.2f;
}
float consoleNightVisionScale(int duration,float a) {
fraction(a);if(duration < 0)throw std::invalid_argument("Negative night vision duration");

	
	if (duration > (20 * 10))
	{
		return 1.0f;
	}
	else
	{
		float flash = std::max(0.0f, (float)duration - a);
		return .7f + Mth::sin(flash * PI * .05f) * .3f; // was:  .7 + sin(flash*pi*0.2) * .3
	}
}
void LightFlicker::tick(const std::array<double,8>& randomValues) {
for(double value:randomValues)if(!std::isfinite(value) || value < 0 || value >= 1)throw std::invalid_argument("Invalid flicker random sample");

    blrt += (float)((randomValues[0] - randomValues[1]) * randomValues[2] * randomValues[3]);
    blgt += (float)((randomValues[4] - randomValues[5]) * randomValues[6] * randomValues[7]);
    blrt *= 0.9;
    blgt *= 0.9;
    blr += (blrt - blr) * 1;
    blg += (blgt - blg) * 1;
    
}
Lightmap buildConsoleLightmap(const LightmapInput& input) {
validDimension(input.dimension);fraction(input.skyDarken);fraction(input.partialTick);
    if(!std::isfinite(input.flicker) || std::abs(input.flicker)>10 || input.nightVisionTicks < -1)throw std::invalid_argument("Invalid lightmap effect input");
float brightnessRamp[16];

    float ambientLight = input.dimension == -1 ? 0.10f : 0.00f;
    for (int i = 0; i <= 15; i++)
	{
        float v = (1 - i / (float) (15));
        brightnessRamp[i] = ((1 - v) / (v * 3 + 1)) * (1 - ambientLight) + ambientLight;
    }

const float skyDarken1=input.skyDarken, blr=input.flicker;Lightmap pixels;
for(int i=0;i<256;++i){
float darken = skyDarken1 * 0.95f + 0.05f;
			float sky = brightnessRamp[i / 16] * darken;
			float block = brightnessRamp[i % 16] * (blr * 0.1f + 1.5f);

			if (input.lightning)
			{
				sky = brightnessRamp[i / 16];
			}

			float rs = sky * (skyDarken1 * 0.65f + 0.35f);
			float gs = sky * (skyDarken1 * 0.65f + 0.35f);
			float bs = sky;

			float rb = block;
			float gb = block * ((block * 0.6f + 0.4f) * 0.6f + 0.4f);
			float bb = block * ((block * block) * 0.6f + 0.4f);

			float _r = (rs + rb);
			float _g = (gs + gb);
			float _b = (bs + bb);

			_r = _r * 0.96f + 0.03f;
			_g = _g * 0.96f + 0.03f;
			_b = _b * 0.96f + 0.03f;

			if (input.dimension == 1)
			{
				_r = (0.22f + rb * 0.75f);
				_g = (0.28f + gb * 0.75f);
				_b = (0.25f + bb * 0.75f);
			}

			if (input.nightVisionTicks >= 0)
			{
				float scale = consoleNightVisionScale(input.nightVisionTicks, input.partialTick);
				{
					float dist = 1.0f / _r;
					if (dist > (1.0f / _g))
					{
						dist = (1.0f / _g);
					}
					if (dist > (1.0f / _b))
					{
						dist = (1.0f / _b);
					}
					_r = _r * (1.0f - scale) + (_r * dist) * scale;
					_g = _g * (1.0f - scale) + (_g * dist) * scale;
					_b = _b * (1.0f - scale) + (_b * dist) * scale;
				}
			}

			float brightness = 0.0f;	// 4J - TODO - was mc->options->gamma;
			if (_r > 1) _r = 1;
			if (_g > 1) _g = 1;
			if (_b > 1) _b = 1;

			float ir = 1 - _r;
			float ig = 1 - _g;
			float ib = 1 - _b;
			ir = 1 - (ir * ir * ir * ir);
			ig = 1 - (ig * ig * ig * ig);
			ib = 1 - (ib * ib * ib * ib);
			_r = _r * (1 - brightness) + ir * brightness;
			_g = _g * (1 - brightness) + ig * brightness;
			_b = _b * (1 - brightness) + ib * brightness;


			_r = _r * 0.96f + 0.03f;
			_g = _g * 0.96f + 0.03f;
			_b = _b * 0.96f + 0.03f;


			if (_r > 1) _r = 1;
			if (_g > 1) _g = 1;
			if (_b > 1) _b = 1;
			if (_r < 0) _r = 0;
			if (_g < 0) _g = 0;
			if (_b < 0) _b = 0;

			int a = 255;
			int r = (int) (_r * 255);
			int g = (int) (_g * 255);
			int b = (int) (_b * 255);
pixels[i] = {std::uint8_t(r), std::uint8_t(g), std::uint8_t(b), std::uint8_t(a)};
}
return pixels;
}
}
