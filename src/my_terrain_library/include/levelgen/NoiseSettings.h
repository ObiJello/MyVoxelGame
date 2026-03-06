#pragma once

namespace minecraft {
namespace levelgen {

class NoiseSettings {
public:
    // Constants for different dimensions
    static const NoiseSettings OVERWORLD_NOISE_SETTINGS;
    static const NoiseSettings NETHER_NOISE_SETTINGS;
    static const NoiseSettings END_NOISE_SETTINGS;
    static const NoiseSettings CAVES_NOISE_SETTINGS;
    static const NoiseSettings FLOATING_ISLANDS_NOISE_SETTINGS;

    // Constructor
    NoiseSettings(int minY, int height, int noiseSizeHorizontal, int noiseSizeVertical);

    // Factory method with validation
    static NoiseSettings create(int minY, int height, int noiseSizeHorizontal, int noiseSizeVertical);

    // Getters
    int minY() const { return m_minY; }
    int height() const { return m_height; }
    int noiseSizeHorizontal() const { return m_noiseSizeHorizontal; }
    int noiseSizeVertical() const { return m_noiseSizeVertical; }

    // Cell dimensions (QuartPos.toBlock() = value * 4)
    int getCellHeight() const;
    int getCellWidth() const;

private:
    int m_minY;
    int m_height;
    int m_noiseSizeHorizontal;
    int m_noiseSizeVertical;

    // Validation helper
    static void guardY(const NoiseSettings& settings);
};

} // namespace levelgen
} // namespace minecraft
