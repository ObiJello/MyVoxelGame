#pragma once

#include "world/biome/Climate.h"
#include "world/biome/Biomes.h"
#include "levelgen/DensityFunction.h"
#include <vector>
#include <utility>
#include <functional>

namespace minecraft {
namespace world {
namespace biome {

/**
 * OverworldBiomeBuilder - Builds the overworld biome parameter list
 * Reference: net/minecraft/world/level/biome/OverworldBiomeBuilder.java
 *
 * This class defines all the biome selection logic for the overworld,
 * including temperature/humidity/continentalness/erosion/weirdness mappings.
 */
class OverworldBiomeBuilder {
public:
    // Reference: OverworldBiomeBuilder.java lines 24-37
    static constexpr float VALLEY_SIZE = 0.05F;
    static constexpr float LOW_START = 0.26666668F;
    static constexpr float HIGH_START = 0.4F;
    static constexpr float HIGH_END = 0.93333334F;
    static constexpr float PEAK_SIZE = 0.1F;
    static constexpr float PEAK_START = 0.56666666F;
    static constexpr float PEAK_END = 0.7666667F;
    static constexpr float NEAR_INLAND_START = -0.11F;
    static constexpr float MID_INLAND_START = 0.03F;
    static constexpr float FAR_INLAND_START = 0.3F;
    static constexpr float EROSION_INDEX_1_START = -0.78F;
    static constexpr float EROSION_INDEX_2_START = -0.375F;
    static constexpr float EROSION_DEEP_DARK_DRYNESS_THRESHOLD = -0.225F;
    static constexpr float DEPTH_DEEP_DARK_DRYNESS_THRESHOLD = 0.9F;

private:
    // Parameter ranges - Reference: OverworldBiomeBuilder.java lines 38-51
    Climate::Parameter m_fullRange;
    Climate::Parameter m_temperatures[5];
    Climate::Parameter m_humidities[5];
    Climate::Parameter m_erosions[7];

    Climate::Parameter m_frozenRange;
    Climate::Parameter m_unfrozenRange;

    Climate::Parameter m_mushroomFieldsContinentalness;
    Climate::Parameter m_deepOceanContinentalness;
    Climate::Parameter m_oceanContinentalness;
    Climate::Parameter m_coastContinentalness;
    Climate::Parameter m_inlandContinentalness;
    Climate::Parameter m_nearInlandContinentalness;
    Climate::Parameter m_midInlandContinentalness;
    Climate::Parameter m_farInlandContinentalness;

    // Biome lookup tables - Reference: OverworldBiomeBuilder.java lines 52-57
    BiomeKey m_oceans[2][5];
    BiomeKey m_middleBiomes[5][5];
    BiomeKey m_middleBiomesVariant[5][5];
    BiomeKey m_plateauBiomes[5][5];
    BiomeKey m_plateauBiomesVariant[5][5];
    BiomeKey m_shatteredBiomes[5][5];

public:
    /**
     * Constructor - initializes all parameter ranges and biome tables
     * Reference: OverworldBiomeBuilder.java lines 59-76
     */
    OverworldBiomeBuilder();

    /**
     * Get spawn target parameters
     * Reference: OverworldBiomeBuilder.java lines 78-82
     */
    std::vector<Climate::ParameterPoint> spawnTarget() const;

    /**
     * Add all biomes to the consumer
     * Reference: OverworldBiomeBuilder.java lines 84-92
     */
    void addBiomes(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer) const;

    /**
     * Check if a position is in the deep dark region
     * Reference: OverworldBiomeBuilder.java lines 411-413
     */
    static bool isDeepDarkRegion(
        const density::DensityFunction* erosion,
        const density::DensityFunction* depth,
        const density::DensityFunction::FunctionContext& context
    );

private:
    // Biome addition methods
    void addOffCoastBiomes(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer) const;
    void addInlandBiomes(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer) const;
    void addUndergroundBiomes(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer) const;

    void addPeaks(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer, const Climate::Parameter& weirdness) const;
    void addHighSlice(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer, const Climate::Parameter& weirdness) const;
    void addMidSlice(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer, const Climate::Parameter& weirdness) const;
    void addLowSlice(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer, const Climate::Parameter& weirdness) const;
    void addValleys(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer, const Climate::Parameter& weirdness) const;

    // Biome picker methods
    BiomeKey pickMiddleBiome(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const;
    BiomeKey pickMiddleBiomeOrBadlandsIfHot(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const;
    BiomeKey pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const;
    BiomeKey maybePickWindsweptSavannaBiome(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness, const BiomeKey& underlyingBiome) const;
    BiomeKey pickShatteredCoastBiome(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const;
    BiomeKey pickBeachBiome(int temperatureIndex, int humidityIndex) const;
    BiomeKey pickBadlandsBiome(int humidityIndex, const Climate::Parameter& weirdness) const;
    BiomeKey pickPlateauBiome(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const;
    BiomeKey pickPeakBiome(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const;
    BiomeKey pickSlopeBiome(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const;
    BiomeKey pickShatteredBiome(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const;

    // Helper methods for adding biomes with different depth ranges
    void addSurfaceBiome(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer,
                        const Climate::Parameter& temperature, const Climate::Parameter& humidity,
                        const Climate::Parameter& continentalness, const Climate::Parameter& erosion,
                        const Climate::Parameter& weirdness, float offset, const BiomeKey& biome) const;

    void addUndergroundBiome(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer,
                            const Climate::Parameter& temperature, const Climate::Parameter& humidity,
                            const Climate::Parameter& continentalness, const Climate::Parameter& erosion,
                            const Climate::Parameter& weirdness, float offset, const BiomeKey& biome) const;

    void addBottomBiome(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer,
                       const Climate::Parameter& temperature, const Climate::Parameter& humidity,
                       const Climate::Parameter& continentalness, const Climate::Parameter& erosion,
                       const Climate::Parameter& weirdness, float offset, const BiomeKey& biome) const;
};

} // namespace biome
} // namespace world
} // namespace minecraft
