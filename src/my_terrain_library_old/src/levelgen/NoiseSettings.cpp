#include "levelgen/NoiseSettings.h"
#include <stdexcept>
#include <string>

namespace minecraft {
namespace levelgen {

// DimensionType constants from Java
static const int DIMENSION_TYPE_MIN_Y = -2032;
static const int DIMENSION_TYPE_MAX_Y = 2031;

// Static constant definitions
const NoiseSettings NoiseSettings::OVERWORLD_NOISE_SETTINGS = NoiseSettings(-64, 384, 1, 2);
const NoiseSettings NoiseSettings::NETHER_NOISE_SETTINGS = NoiseSettings(0, 128, 1, 2);
const NoiseSettings NoiseSettings::END_NOISE_SETTINGS = NoiseSettings(0, 128, 2, 1);
const NoiseSettings NoiseSettings::CAVES_NOISE_SETTINGS = NoiseSettings(-64, 192, 1, 2);
const NoiseSettings NoiseSettings::FLOATING_ISLANDS_NOISE_SETTINGS = NoiseSettings(0, 256, 2, 1);

NoiseSettings::NoiseSettings(int minY, int height, int noiseSizeHorizontal, int noiseSizeVertical)
    : m_minY(minY)
    , m_height(height)
    , m_noiseSizeHorizontal(noiseSizeHorizontal)
    , m_noiseSizeVertical(noiseSizeVertical)
{
}

NoiseSettings NoiseSettings::create(int minY, int height, int noiseSizeHorizontal, int noiseSizeVertical) {
    NoiseSettings settings(minY, height, noiseSizeHorizontal, noiseSizeVertical);
    guardY(settings);
    return settings;
}

void NoiseSettings::guardY(const NoiseSettings& settings) {
    // min_y + height cannot be higher than DimensionType.MAX_Y + 1
    if (settings.minY() + settings.height() > DIMENSION_TYPE_MAX_Y + 1) {
        throw std::invalid_argument(
            "min_y + height cannot be higher than: " + std::to_string(DIMENSION_TYPE_MAX_Y + 1)
        );
    }

    // height has to be a multiple of 16
    if (settings.height() % 16 != 0) {
        throw std::invalid_argument("height has to be a multiple of 16");
    }

    // min_y has to be a multiple of 16
    if (settings.minY() % 16 != 0) {
        throw std::invalid_argument("min_y has to be a multiple of 16");
    }
}

int NoiseSettings::getCellHeight() const {
    // QuartPos.toBlock(noiseSizeVertical) = noiseSizeVertical << 2 = noiseSizeVertical * 4
    return m_noiseSizeVertical * 4;
}

int NoiseSettings::getCellWidth() const {
    // QuartPos.toBlock(noiseSizeHorizontal) = noiseSizeHorizontal << 2 = noiseSizeHorizontal * 4
    return m_noiseSizeHorizontal * 4;
}

} // namespace levelgen
} // namespace minecraft
