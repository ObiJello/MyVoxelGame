#pragma once

#include "core/BlockPos.h"
#include "world/biome/Biomes.h"
#include <cstdint>
#include <string>
#include <set>
#include <vector>

namespace minecraft {
namespace world {
namespace biome {

/**
 * Precipitation - Type of precipitation in a biome
 * Reference: Biome.Precipitation enum
 */
enum class Precipitation {
    NONE,
    RAIN,
    SNOW
};

/**
 * TemperatureModifier - Modifier for biome temperature calculations
 * Reference: Biome.TemperatureModifier enum
 */
enum class TemperatureModifier {
    NONE,
    FROZEN  // For frozen biomes like frozen ocean
};

/**
 * ClimateSettings - Climate parameters for a biome
 * Reference: Biome.ClimateSettings
 */
struct ClimateSettings {
    bool hasPrecipitation;
    float temperature;
    TemperatureModifier temperatureModifier;
    float downfall;

    ClimateSettings()
        : hasPrecipitation(true)
        , temperature(0.8f)
        , temperatureModifier(TemperatureModifier::NONE)
        , downfall(0.4f)
    {}

    ClimateSettings(bool hasPrecip, float temp, TemperatureModifier modifier, float down)
        : hasPrecipitation(hasPrecip)
        , temperature(temp)
        , temperatureModifier(modifier)
        , downfall(down)
    {}
};

/**
 * Biome - Represents a biome with climate and generation settings
 *
 * Reference: net/minecraft/world/level/biome/Biome.java
 */
class Biome {
private:
    int32_t m_id;
    std::string m_name;
    ClimateSettings m_climateSettings;

    // Constants from Biome.java
    static constexpr float SNOW_TEMPERATURE_THRESHOLD = 0.15f;

public:
    Biome()
        : m_id(0)
        , m_name("minecraft:plains")
        , m_climateSettings()
    {}

    Biome(int32_t id, const std::string& name)
        : m_id(id)
        , m_name(name)
        , m_climateSettings()
    {}

    // Constructor with just name (for auto-creation in Biomes::get)
    Biome(const std::string& name)
        : m_id(0)
        , m_name(name)
        , m_climateSettings()
    {}

    Biome(int32_t id, const std::string& name, const ClimateSettings& climate)
        : m_id(id)
        , m_name(name)
        , m_climateSettings(climate)
    {}

    int32_t getId() const { return m_id; }
    const std::string& getName() const { return m_name; }
    const ClimateSettings& getClimateSettings() const { return m_climateSettings; }

    /**
     * Get base temperature of the biome
     * Reference: Biome.java getBaseTemperature()
     */
    float getBaseTemperature() const {
        return m_climateSettings.temperature;
    }

    /**
     * Get temperature at a specific position
     * Reference: Biome.java getTemperature(BlockPos pos)
     *
     * For FROZEN biomes, uses noise to create warmer patches where ice melts.
     * Temperature also decreases with altitude above sea level.
     * Implementation in Biome.cpp
     */
    float getTemperature(const core::BlockPos& pos, int32_t seaLevel = 64) const;

    /**
     * Check if the biome is cold enough to snow at a position
     * Reference: Biome.java coldEnoughToSnow(BlockPos pos, int seaLevel)
     */
    bool coldEnoughToSnow(const core::BlockPos& pos, int32_t seaLevel = 64) const {
        return getTemperature(pos, seaLevel) < SNOW_TEMPERATURE_THRESHOLD;
    }

    /**
     * Check if the biome should have precipitation
     * Reference: Biome.java hasPrecipitation()
     */
    bool hasPrecipitation() const {
        return m_climateSettings.hasPrecipitation;
    }

    /**
     * Check if this biome matches the given biome name
     * Reference: Holder.is() in Java
     */
    bool is(const char* biomeName) const {
        return m_name == biomeName;
    }

    bool is(const std::string& biomeName) const {
        return m_name == biomeName;
    }

    /**
     * Get precipitation type at a position
     * Reference: Biome.java getPrecipitationAt(BlockPos pos, int seaLevel)
     */
    Precipitation getPrecipitationAt(const core::BlockPos& pos, int32_t seaLevel = 64) const {
        if (!m_climateSettings.hasPrecipitation) {
            return Precipitation::NONE;
        }
        if (coldEnoughToSnow(pos, seaLevel)) {
            return Precipitation::SNOW;
        }
        return Precipitation::RAIN;
    }

    /**
     * Static factory methods for common biomes
     */
    static Biome plains() {
        return Biome(1, "minecraft:plains", ClimateSettings(true, 0.8f, TemperatureModifier::NONE, 0.4f));
    }

    static Biome desert() {
        return Biome(2, "minecraft:desert", ClimateSettings(false, 2.0f, TemperatureModifier::NONE, 0.0f));
    }

    static Biome taiga() {
        return Biome(5, "minecraft:taiga", ClimateSettings(true, 0.25f, TemperatureModifier::NONE, 0.8f));
    }

    static Biome snowyTaiga() {
        return Biome(30, "minecraft:snowy_taiga", ClimateSettings(true, -0.5f, TemperatureModifier::NONE, 0.4f));
    }

    static Biome frozenOcean() {
        return Biome(10, "minecraft:frozen_ocean", ClimateSettings(true, 0.0f, TemperatureModifier::FROZEN, 0.5f));
    }
};

// BiomeHolder is defined in Biomes.h

} // namespace biome
} // namespace world
} // namespace minecraft

// Forward declarations in global namespace scope
namespace minecraft { namespace levelgen { namespace placement { class PlacedFeature; } } }
namespace minecraft { namespace levelgen { namespace carver { class ConfiguredCarverBase; } } }

namespace minecraft {
namespace world {
namespace biome {

/**
 * BiomeGenerationSettings - Tracks features and carvers for a biome
 * Reference: net/minecraft/world/level/biome/BiomeGenerationSettings.java
 *
 * This class maintains the set of features and carvers that can generate in a specific biome.
 * BiomeFilter uses hasFeature() to check if a feature should place in a biome.
 */
class BiomeGenerationSettings {
private:
    // Set of features that can generate in this biome
    // Reference: BiomeGenerationSettings.java line 44 featureSet
    std::set<const ::minecraft::levelgen::placement::PlacedFeature*> m_featureSet;

    // List of carvers for this biome
    // Reference: BiomeGenerationSettings.java line 41 carvers
    std::vector<::minecraft::levelgen::carver::ConfiguredCarverBase*> m_carvers;

public:
    BiomeGenerationSettings() = default;

    /**
     * Add a feature to this biome's generation settings
     */
    void addFeature(const ::minecraft::levelgen::placement::PlacedFeature* feature) {
        if (feature) {
            m_featureSet.insert(feature);
        }
    }

    /**
     * Check if this biome has the specified feature
     * Reference: BiomeGenerationSettings.java hasFeature() lines 65-67
     *
     * @param feature The feature to check
     * @return true if this biome contains the feature
     */
    bool hasFeature(const ::minecraft::levelgen::placement::PlacedFeature* feature) const {
        return m_featureSet.find(feature) != m_featureSet.end();
    }

    /**
     * Get all features in this biome
     */
    const std::set<const ::minecraft::levelgen::placement::PlacedFeature*>& features() const {
        return m_featureSet;
    }

    /**
     * Clear all features
     */
    void clear() {
        m_featureSet.clear();
        m_carvers.clear();
    }

    /**
     * Add a carver to this biome's generation settings
     * Reference: BiomeGenerationSettings.Builder.addCarver()
     */
    void addCarver(::minecraft::levelgen::carver::ConfiguredCarverBase* carver) {
        if (carver) {
            m_carvers.push_back(carver);
        }
    }

    /**
     * Get all carvers for this biome
     * Reference: BiomeGenerationSettings.java getCarvers() line 53-55
     */
    const std::vector<::minecraft::levelgen::carver::ConfiguredCarverBase*>& getCarvers() const {
        return m_carvers;
    }

    /**
     * Empty settings singleton
     * Reference: BiomeGenerationSettings.java EMPTY
     */
    static const BiomeGenerationSettings& empty() {
        static BiomeGenerationSettings emptySettings;
        return emptySettings;
    }
};

} // namespace biome
} // namespace world
} // namespace minecraft
