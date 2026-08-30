#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Reference: net/minecraft/world/level/levelgen/structure/placement/
//   StructurePlacement.java, RandomSpreadStructurePlacement.java,
//   ConcentricRingsStructurePlacement.java, RandomSpreadType.java
//
// All structure placement RNG is LegacyRandomSource (java.util.Random LCG),
// never Xoroshiro.

namespace minecraft {
namespace levelgen {
namespace structure {

class ChunkGeneratorStructureState;

// Reference: StructurePlacement.FrequencyReductionMethod
enum class FrequencyReductionMethod {
    DEFAULT,        // setLargeFeatureWithSalt(seed, salt, sx, sz); nextFloat() < f
    LEGACY_TYPE_1,  // pillager outpost: setSeed((cx ^ cz<<4) ^ seed); nextInt(); nextInt(1/f)==0
    LEGACY_TYPE_2,  // buried treasure: setLargeFeatureWithSalt(seed, sx, sz, 10387320); nextFloat() < f
    LEGACY_TYPE_3,  // mineshaft: setLargeFeatureSeed(seed, sx, sz); nextDouble() < f
};

// Reference: RandomSpreadType.java
enum class RandomSpreadType {
    LINEAR,      // nextInt(limit)
    TRIANGULAR,  // (nextInt(limit) + nextInt(limit)) / 2
};

// Reference: StructurePlacement.ExclusionZone (record)
struct ExclusionZone {
    std::string otherSetName;  // resolved through the structure-set registry
    int32_t chunkCount;
};

/**
 * Reference: StructurePlacement.java
 */
class StructurePlacement {
public:
    StructurePlacement(int32_t locateOffsetX, int32_t locateOffsetY, int32_t locateOffsetZ,
                       FrequencyReductionMethod frequencyReductionMethod, float frequency,
                       int32_t salt, std::optional<ExclusionZone> exclusionZone)
        : m_locateOffsetX(locateOffsetX), m_locateOffsetY(locateOffsetY), m_locateOffsetZ(locateOffsetZ),
          m_frequencyReductionMethod(frequencyReductionMethod), m_frequency(frequency),
          m_salt(salt), m_exclusionZone(std::move(exclusionZone)) {}
    virtual ~StructurePlacement() = default;

    int32_t salt() const { return m_salt; }
    float frequency() const { return m_frequency; }
    FrequencyReductionMethod frequencyReductionMethod() const { return m_frequencyReductionMethod; }
    const std::optional<ExclusionZone>& exclusionZone() const { return m_exclusionZone; }

    /**
     * Reference: StructurePlacement.java isStructureChunk() - placement grid
     * check, then frequency reduction, then exclusion zone.
     */
    bool isStructureChunk(const ChunkGeneratorStructureState& state, int32_t sourceX, int32_t sourceZ) const;

    // Reference: applyAdditionalChunkRestrictions()
    bool applyAdditionalChunkRestrictions(int32_t sourceX, int32_t sourceZ, int64_t levelSeed) const;
    // Reference: applyInteractionsWithOtherStructures()
    bool applyInteractionsWithOtherStructures(const ChunkGeneratorStructureState& state,
                                              int32_t sourceX, int32_t sourceZ) const;

    virtual bool isPlacementChunk(const ChunkGeneratorStructureState& state,
                                  int32_t sourceX, int32_t sourceZ) const = 0;

private:
    int32_t m_locateOffsetX;
    int32_t m_locateOffsetY;
    int32_t m_locateOffsetZ;
    FrequencyReductionMethod m_frequencyReductionMethod;
    float m_frequency;
    int32_t m_salt;
    std::optional<ExclusionZone> m_exclusionZone;
};

/**
 * Reference: RandomSpreadStructurePlacement.java
 */
class RandomSpreadStructurePlacement : public StructurePlacement {
public:
    RandomSpreadStructurePlacement(int32_t locateOffsetX, int32_t locateOffsetY, int32_t locateOffsetZ,
                                   FrequencyReductionMethod frequencyReductionMethod, float frequency,
                                   int32_t salt, std::optional<ExclusionZone> exclusionZone,
                                   int32_t spacing, int32_t separation, RandomSpreadType spreadType)
        : StructurePlacement(locateOffsetX, locateOffsetY, locateOffsetZ, frequencyReductionMethod,
                             frequency, salt, std::move(exclusionZone)),
          m_spacing(spacing), m_separation(separation), m_spreadType(spreadType) {}

    int32_t spacing() const { return m_spacing; }
    int32_t separation() const { return m_separation; }

    /**
     * Reference: getPotentialStructureChunk() - the candidate chunk for the
     * spacing-grid cell containing (sourceX, sourceZ).
     */
    std::pair<int32_t, int32_t> getPotentialStructureChunk(int64_t seed, int32_t sourceX, int32_t sourceZ) const;

    bool isPlacementChunk(const ChunkGeneratorStructureState& state,
                          int32_t sourceX, int32_t sourceZ) const override;

private:
    int32_t m_spacing;
    int32_t m_separation;
    RandomSpreadType m_spreadType;
};

/**
 * Reference: ConcentricRingsStructurePlacement.java (strongholds)
 */
class ConcentricRingsStructurePlacement : public StructurePlacement {
public:
    ConcentricRingsStructurePlacement(int32_t locateOffsetX, int32_t locateOffsetY, int32_t locateOffsetZ,
                                      FrequencyReductionMethod frequencyReductionMethod, float frequency,
                                      int32_t salt, std::optional<ExclusionZone> exclusionZone,
                                      int32_t distance, int32_t spread, int32_t count,
                                      std::string preferredBiomesTag)
        : StructurePlacement(locateOffsetX, locateOffsetY, locateOffsetZ, frequencyReductionMethod,
                             frequency, salt, std::move(exclusionZone)),
          m_distance(distance), m_spread(spread), m_count(count),
          m_preferredBiomesTag(std::move(preferredBiomesTag)) {}

    int32_t distance() const { return m_distance; }
    int32_t spread() const { return m_spread; }
    int32_t count() const { return m_count; }
    const std::string& preferredBiomesTag() const { return m_preferredBiomesTag; }

    bool isPlacementChunk(const ChunkGeneratorStructureState& state,
                          int32_t sourceX, int32_t sourceZ) const override;

private:
    int32_t m_distance;
    int32_t m_spread;
    int32_t m_count;
    std::string m_preferredBiomesTag;  // e.g. "#minecraft:stronghold_biased_to"
};

} // namespace structure
} // namespace levelgen
} // namespace minecraft
