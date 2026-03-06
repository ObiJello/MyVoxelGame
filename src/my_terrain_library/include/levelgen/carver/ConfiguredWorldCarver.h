#pragma once

#include "levelgen/carver/WorldCarver.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/carver/CarvingContext.h"
#include "levelgen/carver/CarvingMask.h"
#include "levelgen/Aquifer.h"
#include "world/ChunkPos.h"
#include "world/IChunk.h"
#include "random/XoroshiroRandomSource.h"
#include "random/LegacyRandomSource.h"
#include <functional>
#include <memory>

// Reference: net/minecraft/world/level/levelgen/carver/ConfiguredWorldCarver.java

namespace minecraft {
namespace levelgen {
namespace carver {

/**
 * ConfiguredCarverBase - Non-templated base class for configured carvers
 * This allows storing different carver types in a single container.
 *
 * Note: Uses LegacyRandomSource for carving operations (matching Java behavior)
 */
class ConfiguredCarverBase {
public:
    virtual ~ConfiguredCarverBase() = default;

    /**
     * Check if this chunk should start carving
     * Reference: ConfiguredWorldCarver.java lines 25-27
     */
    virtual bool isStartChunk(LegacyRandomSource& random) const = 0;

    /**
     * Carve the given chunk using LegacyRandomSource
     * Reference: ConfiguredWorldCarver.java lines 29-31
     */
    virtual bool carve(
        CarvingContext& context,
        ::world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        LegacyRandomSource& random,
        Aquifer* aquifer,
        const ::world::ChunkPos& sourceChunkPos,
        CarvingMask& mask
    ) = 0;
};

/**
 * ConfiguredWorldCarver - A world carver with its configuration
 * Reference: ConfiguredWorldCarver.java
 *
 * This wraps a WorldCarver with its specific configuration, providing
 * a clean interface for carving operations.
 *
 * Template parameter WC is the configuration type
 *
 * Inherits from ConfiguredCarverBase for polymorphic storage.
 */
template<typename WC>
class ConfiguredWorldCarver : public ConfiguredCarverBase {
private:
    WorldCarver<WC>* m_worldCarver;
    WC m_config;

public:
    /**
     * Constructor
     * Reference: ConfiguredWorldCarver.java record constructor
     */
    ConfiguredWorldCarver(WorldCarver<WC>* worldCarver, const WC& config)
        : m_worldCarver(worldCarver)
        , m_config(config)
    {}

    /**
     * Check if this chunk should start carving (LegacyRandomSource version)
     * Reference: ConfiguredWorldCarver.java lines 25-27
     * This is the version used by Java's carving system.
     */
    bool isStartChunk(LegacyRandomSource& random) const override {
        return m_worldCarver->isStartChunk(m_config, random);
    }

    /**
     * Check if this chunk should start carving (XoroshiroRandomSource version)
     * For backwards compatibility.
     */
    bool isStartChunk(XoroshiroRandomSource& random) const {
        return m_worldCarver->isStartChunk(m_config, random);
    }

    /**
     * Carve the given chunk using LegacyRandomSource
     * Reference: ConfiguredWorldCarver.java lines 29-31
     * This is the version used by Java's carving system.
     */
    bool carve(
        CarvingContext& context,
        ::world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        LegacyRandomSource& random,
        Aquifer* aquifer,
        const ::world::ChunkPos& sourceChunkPos,
        CarvingMask& mask
    ) override {
        return m_worldCarver->carve(
            context,
            m_config,
            chunk,
            biomeGetter,
            random,
            aquifer,
            sourceChunkPos,
            mask
        );
    }

    /**
     * Carve the given chunk using XoroshiroRandomSource
     * For backwards compatibility.
     */
    bool carve(
        CarvingContext& context,
        ::world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        XoroshiroRandomSource& random,
        Aquifer* aquifer,
        const ::world::ChunkPos& sourceChunkPos,
        CarvingMask& mask
    ) {
        return m_worldCarver->carve(
            context,
            m_config,
            chunk,
            biomeGetter,
            random,
            aquifer,
            sourceChunkPos,
            mask
        );
    }

    /**
     * Get the world carver
     */
    WorldCarver<WC>* worldCarver() const { return m_worldCarver; }

    /**
     * Get the configuration
     */
    const WC& config() const { return m_config; }
};

/**
 * Type aliases for common configured carvers
 */
using ConfiguredCaveCarver = ConfiguredWorldCarver<CaveCarverConfiguration>;
using ConfiguredCanyonCarver = ConfiguredWorldCarver<CanyonCarverConfiguration>;

} // namespace carver
} // namespace levelgen
} // namespace minecraft
