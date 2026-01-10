#include "synth/PerlinSimplexNoise.h"

namespace minecraft {
namespace synth {

// Static member initialization
std::unique_ptr<PerlinSimplexNoise> BiomeInfoNoise::s_instance = nullptr;
bool BiomeInfoNoise::s_initialized = false;

} // namespace synth
} // namespace minecraft
