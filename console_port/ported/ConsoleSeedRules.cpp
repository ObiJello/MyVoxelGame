// Generated from BiomeSource::getFracs and getIsMatch.
#include "ConsoleSeed.h"
#include <cmath>
#include <stdexcept>
namespace console {
std::array<float,23> consoleBiomeFractions(std::span<const std::uint8_t> indices){
 if(indices.empty() || indices.size()>40000)throw std::invalid_argument("Invalid seed biome sample size");
 for(auto id:indices)if(id>=23)throw std::invalid_argument("Invalid seed biome ID");
 std::array<float,23> fracs;

	for( int i = 0; i < 23; i++ )
	{
		fracs[i] = 0.0f;
	}

	for( int i = 0; i < indices.size(); i++ )
	{
		fracs[indices[i]] += 1.0f;
	}

	for( int i = 0; i < 23; i++ )
	{
		fracs[i] /= (float)(indices.size());
	}
return fracs;
}
bool consoleSeedMatches(std::array<float,23> frac){
 for(float f:frac)if(!std::isfinite(f) || f<0 || f>1)throw std::invalid_argument("Invalid biome fraction");

	// A true for a particular biome type here marks it as one that *has* to be present
	static const bool critical[23] = { 
										true,	// ocean
										true,	// plains
										true,	// desert
										false,	// extreme hills
										true,	// forest
										true,	// taiga
										true,	// swamps
										false,	// river
										false,	// hell
										false,	// end biome
										false,	// frozen ocean
										false,	// frozen river
										false,	// ice flats
										false,	// ice mountains
										true,	// mushroom island / shore
										false,  // mushroom shore (combined with above)
										false,	// beach
										false,	// desert hills (combined with desert)
										false,	// forest hills (combined with forest)
										false,	// taiga hills (combined with taga)
										false,	// small extreme hills
										true,	// jungle
										false,	// jungle hills (combined with jungle)
										};


	// Don't want more than 15% ocean
	if( frac[0] > 0.15f )
	{
		return false;
	}

	// Consider mushroom shore & islands as the same by finding max
	frac[14] = ( ( frac[15] > frac[14] ) ? frac[15] : frac[14] );

	// Merge desert and desert hills
	frac[2] = ( ( frac[17] > frac[2] ) ? frac[17] : frac[2] );

	// Merge forest and forest hills
	frac[4] = ( ( frac[18] > frac[4] ) ? frac[18] : frac[4] );

	// Merge taiga and taiga hills
	frac[5] = ( ( frac[19] > frac[5] ) ? frac[19] : frac[5] );

	// Merge jungle and jungle hills
	frac[21] =  ( ( frac[22] > frac[21] ) ? frac[22] : frac[21] );

	// Loop through all biome types, and:
	// (1) count them
	// (2) give up if one of the critical ones is missing

	int typeCount = 0;
	for( int i = 0; i < 23; i++ )
	{
		// We want to skip some where we have merged with another type
		if(i == 15 || i == 17 || i == 18 || i == 19 || i == 22) continue;

		// Consider 0.1% as being "present" - this equates an area of about 3 chunks
		if( frac[i] > 0.001f )
		{
			typeCount++;
		}
		else
		{
			// If a critical biome is missing, just give up
			if( critical[i] )
			{
				return false;
			}
		}
	}

	// Consider as suitable if we've got all the critical ones, and in total 9 or more - currently there's 8 critical so this just forces at least 1 more others
	return ( typeCount >= 9 );

}
}
