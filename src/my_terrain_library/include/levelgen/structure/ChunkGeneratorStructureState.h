#pragma once

#include "levelgen/structure/StructureSet.h"
#include "world/biome/BiomeSource.h"
#include "world/biome/Climate.h"
#include "random/LegacyRandomSource.h"
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

// Reference: net/minecraft/world/level/chunk/ChunkGeneratorStructureState.java

namespace minecraft {
namespace levelgen {
namespace structure {

/**
 * Placement state for one (seed, biome source) pair: the biome-filtered
 * structure-set list, per-structure placements, and precomputed concentric
 * ring positions (strongholds).
 *
 * Reference: ChunkGeneratorStructureState.java. createForNormal uses
 * concentricRingsSeed == levelSeed. Ring positions are computed synchronously
 * here; Java forks the ring RNG EAGERLY in loop order and only runs the biome
 * searches async, so the sequence is identical.
 */
class ChunkGeneratorStructureState {
public:
    /**
     * Reference: createForNormal(). `allSets` must be in registry (alphabetical)
     * order; the filtered possibleStructureSets keeps that order - it is the
     * createStructures iteration order (parity-load-bearing).
     */
    static ChunkGeneratorStructureState createForNormal(
        const world::biome::Climate::Sampler* sampler,
        int64_t levelSeed,
        world::biome::BiomeSource* biomeSource,
        const std::vector<const StructureSet*>& allSets);

    /**
     * Reference: createForFlat() - same biome filter over the given sets (a
     * flat world's structure_overrides, or all sets when it has none), but
     * concentricRingsSeed is 0 instead of the level seed.
     */
    static ChunkGeneratorStructureState createForFlat(
        const world::biome::Climate::Sampler* sampler,
        int64_t levelSeed,
        world::biome::BiomeSource* biomeSource,
        const std::vector<const StructureSet*>& sets);

    const std::vector<const StructureSet*>& possibleStructureSets() const {
        return m_possibleStructureSets;
    }

    // Reference: ensureStructuresGenerated() -> generatePositions()
    void ensureStructuresGenerated();

    using Executor = std::function<void(std::function<void()>)>;
    /**
     * Java runs generateRingPositions as one CompletableFuture.supplyAsync per
     * ring position on Util.backgroundExecutor() (ChunkGeneratorStructureState
     * .java:126-138) and joins the list on first use. The port ran all 128
     * biome searches serially on the worldgen lane: 2.2 s during which no
     * chunk could start (measured 2026-08-29). Each search uses a forked RNG
     * and the ring bookkeeping never reads a search result, so the candidate
     * pre-pass stays serial (bit-exact RNG order) and only the searches fan
     * out. Call once, right after construction, with the terrain pool; the
     * work starts immediately and overlaps world load. Without an executor
     * the positions are computed serially on first use, as before.
     */
    void startRingGeneration(Executor executor);

    /**
     * Reference: getRingPositionsFor(). Returns nullptr when the placement has
     * no generated positions (set filtered out). Positions are chunk coords.
     */
    const std::vector<std::pair<int32_t, int32_t>>* getRingPositionsFor(
        const ConcentricRingsStructurePlacement* placement) const;

    // Reference: hasStructureChunkInRange() (ExclusionZone support)
    bool hasStructureChunkInRange(const std::string& structureSetName,
                                  int32_t sourceX, int32_t sourceZ, int32_t range) const;

    int64_t getLevelSeed() const { return m_levelSeed; }
    world::biome::BiomeSource* biomeSource() const { return m_biomeSource; }
    const world::biome::Climate::Sampler* sampler() const { return m_sampler; }

private:
    ChunkGeneratorStructureState(const world::biome::Climate::Sampler* sampler,
                                 int64_t levelSeed,
                                 world::biome::BiomeSource* biomeSource,
                                 std::vector<const StructureSet*> possibleSets)
        : m_sampler(sampler), m_levelSeed(levelSeed), m_concentricRingsSeed(levelSeed),
          m_biomeSource(biomeSource), m_possibleStructureSets(std::move(possibleSets)) {}

    ChunkGeneratorStructureState(const world::biome::Climate::Sampler* sampler,
                                 int64_t levelSeed, int64_t concentricRingsSeed,
                                 world::biome::BiomeSource* biomeSource,
                                 std::vector<const StructureSet*> possibleSets)
        : m_sampler(sampler), m_levelSeed(levelSeed),
          m_concentricRingsSeed(concentricRingsSeed),
          m_biomeSource(biomeSource), m_possibleStructureSets(std::move(possibleSets)) {}

    // Reference: generateRingPositions(). One async job per placement.
    struct RingJob {
        std::vector<std::pair<int32_t, int32_t>> positions;
        std::atomic<int32_t> remaining{0};
        std::mutex mutex;
        std::condition_variable done;
        void wait();
    };
    std::shared_ptr<RingJob> generateRingPositions(
        const ConcentricRingsStructurePlacement* placement, const Executor& executor) const;

    const world::biome::Climate::Sampler* m_sampler;
    int64_t m_levelSeed;
    int64_t m_concentricRingsSeed;
    world::biome::BiomeSource* m_biomeSource;
    std::vector<const StructureSet*> m_possibleStructureSets;
    bool m_hasGeneratedPositions = false;
    Executor m_executor;
    std::unordered_map<const ConcentricRingsStructurePlacement*,
                       std::shared_ptr<RingJob>> m_ringPositions;
};

/**
 * Reference: BiomeSource.java findBiomeHorizontal(x, y, z, radius, 1, allowed,
 * random, findClosest=false, sampler) - full-square scan at quart resolution
 * with reservoir sampling via random.nextInt(found + 1). Returns true and the
 * winning BLOCK position when a match was found.
 */
bool findBiomeHorizontal(world::biome::BiomeSource& source,
                         int32_t originX, int32_t originY, int32_t originZ,
                         int32_t searchRadius,
                         const std::unordered_set<std::string>& allowed,
                         LegacyRandomSource& random,
                         const world::biome::Climate::Sampler& sampler,
                         int32_t& outBlockX, int32_t& outBlockZ);

} // namespace structure
} // namespace levelgen
} // namespace minecraft
