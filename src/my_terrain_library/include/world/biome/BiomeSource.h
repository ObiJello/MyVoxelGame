#pragma once

#include "world/biome/Climate.h"
#include "world/biome/Biomes.h"
#include <functional>
#include <vector>
#include <set>
#include <string>
#include <memory>

// Reference: net/minecraft/world/level/biome/BiomeSource.java

namespace minecraft {
namespace world {
namespace biome {

/**
 * BiomeSource - Base class for biome generation sources
 * Reference: net/minecraft/world/level/biome/BiomeSource.java
 *
 * BiomeSources determine which biome exists at any given coordinate.
 * Different implementations use different algorithms:
 * - MultiNoiseBiomeSource: Uses noise-based climate parameters (overworld, nether)
 * - TheEndBiomeSource: Uses special end island generation
 * - FixedBiomeSource: Returns a single biome (superflat)
 * - CheckerboardBiomeSource: Alternating pattern (debug)
 */
class BiomeSource {
protected:
    // Set of all possible biomes this source can generate
    std::set<BiomeKey> m_possibleBiomes;

public:
    BiomeSource() = default;
    virtual ~BiomeSource() = default;

    /**
     * Get the biome at a given quart position (4x4x4 block resolution)
     * Reference: BiomeSource.java getNoiseBiome()
     *
     * @param quartX X coordinate divided by 4
     * @param quartY Y coordinate divided by 4
     * @param quartZ Z coordinate divided by 4
     * @param sampler Climate sampler for noise-based biome sources
     * @return The biome key at this position
     *
     * CRITICAL: This is the main biome lookup method called during chunk generation.
     * Quart coordinates are used because biomes have 4x4x4 block resolution.
     */
    virtual BiomeKey getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ,
                                   const Climate::Sampler& sampler) = 0;

    // BiomeResolver: (quartX, quartY, quartZ) -> biome.
    using BiomeResolver = std::function<BiomeKey(int32_t, int32_t, int32_t)>;

    // Reference: 26.3 BiomeSource.createResolverForChunk - the biomes of one
    // quart box, which a source may sample as a whole volume up front (the
    // multi-noise source does; see MultiNoiseBiomeSource). The default asks
    // getNoiseBiome per cell. The sampler must outlive the resolver.
    virtual BiomeResolver createResolverForChunk(const Climate::Sampler& sampler, int32_t minQuartX,
                                                 int32_t minQuartY, int32_t minQuartZ, int32_t quartSizeX,
                                                 int32_t quartSizeY, int32_t quartSizeZ) {
        (void)minQuartX; (void)minQuartY; (void)minQuartZ;
        (void)quartSizeX; (void)quartSizeY; (void)quartSizeZ;
        const Climate::Sampler* bound = &sampler;
        return [this, bound](int32_t quartX, int32_t quartY, int32_t quartZ) {
            return getNoiseBiome(quartX, quartY, quartZ, *bound);
        };
    }

    /**
     * Get all possible biomes this source can generate
     * Reference: BiomeSource.java possibleBiomes()
     */
    const std::set<BiomeKey>& possibleBiomes() const {
        return m_possibleBiomes;
    }

    /**
     * Get spawn target parameters
     * Reference: BiomeSource.java getSpawnTarget()
     */
    virtual std::vector<Climate::ParameterPoint> getSpawnTarget() const {
        return {};
    }

    /**
     * Add a listener for biome resolution (used by server)
     * Reference: BiomeSource.java addDebugInfo()
     */
    virtual void addDebugInfo(std::vector<std::string>& /* info */,
                             int32_t /* x */, int32_t /* y */, int32_t /* z */,
                             const Climate::Sampler& /* sampler */) const {
        // Default: no debug info
    }

protected:
    /**
     * Collect all biomes from a parameter list
     */
    template<typename T>
    void collectBiomes(const std::vector<std::pair<Climate::ParameterPoint, T>>& parameters) {
        for (const auto& pair : parameters) {
            m_possibleBiomes.insert(pair.second);
        }
    }
};

} // namespace biome
} // namespace world
} // namespace minecraft
