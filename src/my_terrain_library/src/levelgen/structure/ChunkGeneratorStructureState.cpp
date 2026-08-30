#include "levelgen/structure/ChunkGeneratorStructureState.h"
#include "levelgen/WorldGenTweaks.h"
#include "world/biome/FixedBiomeSource.h"

#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <stdexcept>

// Reference: net/minecraft/world/level/chunk/ChunkGeneratorStructureState.java,
// net/minecraft/world/level/levelgen/structure/placement/StructurePlacement.java,
// RandomSpreadStructurePlacement.java, ConcentricRingsStructurePlacement.java,
// net/minecraft/world/level/biome/BiomeSource.java (findBiomeHorizontal).

namespace minecraft {
namespace levelgen {
namespace structure {

namespace {

// Java Math.floorDiv for int
inline int32_t floorDiv(int32_t x, int32_t y) {
    int32_t r = x / y;
    if ((x ^ y) < 0 && r * y != x) --r;
    return r;
}

// Reference: RandomSpreadType.evaluate()
int32_t evaluateSpread(RandomSpreadType type, LegacyRandomSource& random, int32_t limit) {
    switch (type) {
        case RandomSpreadType::LINEAR:
            return random.nextInt(limit);
        case RandomSpreadType::TRIANGULAR:
            return (random.nextInt(limit) + random.nextInt(limit)) / 2;
    }
    throw std::runtime_error("unreachable spread type");
}

} // namespace

// ---------------------------------------------------------------------------
// StructurePlacement
// ---------------------------------------------------------------------------

bool StructurePlacement::isStructureChunk(const ChunkGeneratorStructureState& state,
                                          int32_t sourceX, int32_t sourceZ) const {
    return isPlacementChunk(state, sourceX, sourceZ)
        && applyAdditionalChunkRestrictions(sourceX, sourceZ, state.getLevelSeed())
        && applyInteractionsWithOtherStructures(state, sourceX, sourceZ);
}

bool StructurePlacement::applyAdditionalChunkRestrictions(int32_t sourceX, int32_t sourceZ,
                                                          int64_t levelSeed) const {
    if (!(m_frequency < 1.0f)) {
        return true;
    }
    // Reference: StructurePlacement frequency reducers - all LegacyRandomSource(0).
    switch (m_frequencyReductionMethod) {
        case FrequencyReductionMethod::DEFAULT: {
            LegacyRandomSource random(0);
            random.setLargeFeatureWithSalt(levelSeed, m_salt, sourceX, sourceZ);
            return random.nextFloat() < m_frequency;
        }
        case FrequencyReductionMethod::LEGACY_TYPE_1: {
            // legacyPillagerOutpostReducer
            int32_t cx = sourceX >> 4;
            int32_t cz = sourceZ >> 4;
            LegacyRandomSource random(0);
            random.setSeed(static_cast<int64_t>(cx ^ (cz << 4)) ^ levelSeed);
            random.nextInt();
            return random.nextInt(static_cast<int32_t>(1.0f / m_frequency)) == 0;
        }
        case FrequencyReductionMethod::LEGACY_TYPE_2: {
            // legacyArbitrarySaltProbabilityReducer (HIGHLY_ARBITRARY_RANDOM_SALT)
            LegacyRandomSource random(0);
            random.setLargeFeatureWithSalt(levelSeed, sourceX, sourceZ, 10387320);
            return random.nextFloat() < m_frequency;
        }
        case FrequencyReductionMethod::LEGACY_TYPE_3: {
            // legacyProbabilityReducerWithDouble
            LegacyRandomSource random(0);
            random.setLargeFeatureSeed(levelSeed, sourceX, sourceZ);
            return random.nextDouble() < static_cast<double>(m_frequency);
        }
    }
    throw std::runtime_error("unreachable frequency reduction method");
}

bool StructurePlacement::applyInteractionsWithOtherStructures(const ChunkGeneratorStructureState& state,
                                                              int32_t sourceX, int32_t sourceZ) const {
    if (!m_exclusionZone.has_value()) {
        return true;
    }
    return !state.hasStructureChunkInRange(m_exclusionZone->otherSetName, sourceX, sourceZ,
                                           m_exclusionZone->chunkCount);
}

// ---------------------------------------------------------------------------
// RandomSpreadStructurePlacement
// ---------------------------------------------------------------------------

std::pair<int32_t, int32_t> RandomSpreadStructurePlacement::getPotentialStructureChunk(
        int64_t seed, int32_t sourceX, int32_t sourceZ) const {
    // World Properties structure-frequency knob (non-vanilla; 1.0 = exact
    // vanilla grid). Spacing shrinks/grows, separation clamps below it.
    int32_t spacing = m_spacing;
    int32_t separation = m_separation;
    {
        float freq = WorldGenTweaks::get().structureFrequency;
        if (freq != 1.0f) {
            spacing = std::max(1, static_cast<int32_t>(std::lround(m_spacing / freq)));
            separation = std::min(separation, spacing - 1);
            if (separation < 0) separation = 0;
        }
    }
    int32_t spacedGridX = floorDiv(sourceX, spacing);
    int32_t spacedGridZ = floorDiv(sourceZ, spacing);
    LegacyRandomSource random(0);
    random.setLargeFeatureWithSalt(seed, spacedGridX, spacedGridZ, salt());
    int32_t limit = spacing - separation;
    int32_t spreadX = evaluateSpread(m_spreadType, random, limit);
    int32_t spreadZ = evaluateSpread(m_spreadType, random, limit);
    return {spacedGridX * spacing + spreadX, spacedGridZ * spacing + spreadZ};
}

bool RandomSpreadStructurePlacement::isPlacementChunk(const ChunkGeneratorStructureState& state,
                                                      int32_t sourceX, int32_t sourceZ) const {
    auto chunkPos = getPotentialStructureChunk(state.getLevelSeed(), sourceX, sourceZ);
    return chunkPos.first == sourceX && chunkPos.second == sourceZ;
}

// ---------------------------------------------------------------------------
// ConcentricRingsStructurePlacement
// ---------------------------------------------------------------------------

bool ConcentricRingsStructurePlacement::isPlacementChunk(const ChunkGeneratorStructureState& state,
                                                         int32_t sourceX, int32_t sourceZ) const {
    const auto* positions = state.getRingPositionsFor(this);
    if (positions == nullptr) return false;
    for (const auto& pos : *positions) {
        if (pos.first == sourceX && pos.second == sourceZ) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// findBiomeHorizontal
// ---------------------------------------------------------------------------

bool findBiomeHorizontal(world::biome::BiomeSource& source,
                         int32_t originX, int32_t originY, int32_t originZ,
                         int32_t searchRadius,
                         const std::unordered_set<std::string>& allowed,
                         LegacyRandomSource& random,
                         const world::biome::Climate::Sampler& sampler,
                         int32_t& outBlockX, int32_t& outBlockZ) {
    // Reference: FixedBiomeSource.findBiomeHorizontal override - no scan, a
    // uniformly random position in the square (2 draws, X then Z; findClosest
    // =false path). This is a DIFFERENT draw stream from the generic reservoir
    // scan below, so the dispatch must match Java's virtual dispatch exactly.
    if (auto* fixed = dynamic_cast<world::biome::FixedBiomeSource*>(&source)) {
        if (allowed.find(fixed->biome()) == allowed.end()) return false;
        outBlockX = originX - searchRadius + random.nextInt(searchRadius * 2 + 1);
        outBlockZ = originZ - searchRadius + random.nextInt(searchRadius * 2 + 1);
        return true;
    }

    // Reference: BiomeSource.findBiomeHorizontal(..., skipSteps=1, findClosest=false).
    // QuartPos.fromBlock == >> 2; QuartPos.toBlock == << 2.
    int32_t noiseCenterX = originX >> 2;
    int32_t noiseCenterZ = originZ >> 2;
    int32_t noiseRadius = searchRadius >> 2;
    int32_t noiseY = originY >> 2;
    bool haveResult = false;
    int32_t found = 0;

    // findClosest=false: startRadius == noiseRadius, so exactly one iteration
    // scanning the full square, with reservoir sampling on the fork random.
    for (int32_t currentRadius = noiseRadius; currentRadius <= noiseRadius; ++currentRadius) {
        for (int32_t z = -currentRadius; z <= currentRadius; ++z) {
            for (int32_t x = -currentRadius; x <= currentRadius; ++x) {
                int32_t noiseX = noiseCenterX + x;
                int32_t noiseZ = noiseCenterZ + z;
                world::biome::BiomeKey biome = source.getNoiseBiome(noiseX, noiseY, noiseZ, sampler);
                if (allowed.find(biome) == allowed.end()) {
                    continue;
                }
                if (!haveResult || random.nextInt(found + 1) == 0) {
                    outBlockX = noiseX << 2;
                    outBlockZ = noiseZ << 2;
                    haveResult = true;
                }
                ++found;
            }
        }
    }
    return haveResult;
}

// ---------------------------------------------------------------------------
// ChunkGeneratorStructureState
// ---------------------------------------------------------------------------

ChunkGeneratorStructureState ChunkGeneratorStructureState::createForNormal(
        const world::biome::Climate::Sampler* sampler,
        int64_t levelSeed,
        world::biome::BiomeSource* biomeSource,
        const std::vector<const StructureSet*>& allSets) {
    // Reference: createForNormal + hasBiomesForStructureSet - keep any set with
    // at least one structure whose biome tag intersects possibleBiomes().
    const auto& possible = biomeSource->possibleBiomes();
    std::vector<const StructureSet*> filtered;
    for (const StructureSet* set : allSets) {
        bool hasBiomes = false;
        for (const auto& entry : set->structures) {
            const auto& biomes = BiomeTags::resolve(entry.structure->biomesTag);
            for (const auto& biome : biomes) {
                if (possible.count(biome)) { hasBiomes = true; break; }
            }
            if (hasBiomes) break;
        }
        if (hasBiomes) filtered.push_back(set);
    }
    return ChunkGeneratorStructureState(sampler, levelSeed, biomeSource, std::move(filtered));
}

ChunkGeneratorStructureState ChunkGeneratorStructureState::createForFlat(
        const world::biome::Climate::Sampler* sampler,
        int64_t levelSeed,
        world::biome::BiomeSource* biomeSource,
        const std::vector<const StructureSet*>& sets) {
    // Reference: createForFlat - identical hasBiomesForStructureSet filter,
    // but concentricRingsSeed = 0L (flat stronghold rings are seed-invariant).
    const auto& possible = biomeSource->possibleBiomes();
    std::vector<const StructureSet*> filtered;
    for (const StructureSet* set : sets) {
        bool hasBiomes = false;
        for (const auto& entry : set->structures) {
            const auto& biomes = BiomeTags::resolve(entry.structure->biomesTag);
            for (const auto& biome : biomes) {
                if (possible.count(biome)) { hasBiomes = true; break; }
            }
            if (hasBiomes) break;
        }
        if (hasBiomes) filtered.push_back(set);
    }
    return ChunkGeneratorStructureState(sampler, levelSeed, /*concentricRingsSeed=*/0,
                                        biomeSource, std::move(filtered));
}

void ChunkGeneratorStructureState::ensureStructuresGenerated() {
    if (m_hasGeneratedPositions) return;
    m_hasGeneratedPositions = true;

    // Reference: generatePositions() - ring positions only for sets that kept
    // at least one placeable structure (per-structure biome check).
    const auto& possible = m_biomeSource->possibleBiomes();
    for (const StructureSet* set : m_possibleStructureSets) {
        bool hasAnyPlaceableStructures = false;
        for (const auto& entry : set->structures) {
            const auto& biomes = BiomeTags::resolve(entry.structure->biomesTag);
            for (const auto& biome : biomes) {
                if (possible.count(biome)) { hasAnyPlaceableStructures = true; break; }
            }
            if (hasAnyPlaceableStructures) break;
        }
        if (!hasAnyPlaceableStructures) continue;
        if (const auto* rings = dynamic_cast<const ConcentricRingsStructurePlacement*>(set->placement.get())) {
            m_ringPositions.emplace(rings, generateRingPositions(rings, m_executor));
        }
    }
}

void ChunkGeneratorStructureState::startRingGeneration(Executor executor) {
    // OBEY_SERIAL_RINGS=1: keep the old serial path (parity A/B against the
    // [RingPositions] log line).
    m_executor = std::getenv("OBEY_SERIAL_RINGS") ? Executor{} : std::move(executor);
    ensureStructuresGenerated();
}

void ChunkGeneratorStructureState::RingJob::wait() {
    if (remaining.load(std::memory_order_acquire) == 0) return;
    std::unique_lock<std::mutex> lock(mutex);
    done.wait(lock, [this] { return remaining.load(std::memory_order_acquire) == 0; });
}

std::shared_ptr<ChunkGeneratorStructureState::RingJob> ChunkGeneratorStructureState::generateRingPositions(
        const ConcentricRingsStructurePlacement* placement, const Executor& executor) const {
    auto job = std::make_shared<RingJob>();
    int32_t count = placement->count();
    if (count == 0) return job;
    int32_t distance = placement->distance();
    int32_t spread = placement->spread();
    const auto& preferredBiomes = BiomeTags::resolve(placement->preferredBiomesTag());

    // Reference: generateRingPositions(). Java draws each position's angle,
    // radius and forked search RNG from ONE LegacyRandomSource in this exact
    // order, then hands the biome search to a background task. The bookkeeping
    // (angle, spread, circle) never depends on a search result, so this
    // serial pre-pass reproduces the stream bit for bit and the searches can
    // run in any order.
    struct Candidate { int32_t initialX, initialZ; LegacyRandomSource search; };
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<size_t>(count));
    LegacyRandomSource random(0);
    random.setSeed(m_concentricRingsSeed);
    double angle = random.nextDouble() * M_PI * 2.0;
    int32_t positionInCircle = 0;
    int32_t circle = 0;
    for (int32_t i = 0; i < count; ++i) {
        double dist = static_cast<double>(4 * distance + distance * circle * 6)
                    + (random.nextDouble() - 0.5) * static_cast<double>(distance) * 2.5;
        int32_t initialX = static_cast<int32_t>(std::floor(std::cos(angle) * dist + 0.5));
        int32_t initialZ = static_cast<int32_t>(std::floor(std::sin(angle) * dist + 0.5));
        candidates.push_back(Candidate{initialX, initialZ, random.fork()});
        angle += (M_PI * 2.0) / static_cast<double>(spread);
        ++positionInCircle;
        if (positionInCircle == spread) {
            ++circle;
            positionInCircle = 0;
            spread += 2 * spread / (circle + 1);
            spread = std::min(spread, count - i);
            angle += random.nextDouble() * M_PI * 2.0;
        }
    }

    job->positions.resize(candidates.size());
    job->remaining.store(static_cast<int32_t>(candidates.size()), std::memory_order_release);
    auto searchOne = [this, &preferredBiomes, job](size_t index, Candidate& c) {
        int32_t blockX, blockZ;
        std::pair<int32_t, int32_t> result{c.initialX, c.initialZ};
        if (findBiomeHorizontal(*m_biomeSource, c.initialX * 16 + 8, 0, c.initialZ * 16 + 8, 112,
                                preferredBiomes, c.search, *m_sampler, blockX, blockZ)) {
            result = {blockX >> 4, blockZ >> 4};
        }
        job->positions[index] = result;
        if (job->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->done.notify_all();
        }
    };
    if (!executor) {
        for (size_t i = 0; i < candidates.size(); ++i) searchOne(i, candidates[i]);
        return job;
    }
    // The candidate list must outlive every task: shared, tasks index into it.
    auto shared = std::make_shared<std::vector<Candidate>>(std::move(candidates));
    for (size_t i = 0; i < shared->size(); ++i) {
        executor([searchOne, shared, i]() { searchOne(i, (*shared)[i]); });
    }
    return job;
}

const std::vector<std::pair<int32_t, int32_t>>* ChunkGeneratorStructureState::getRingPositionsFor(
        const ConcentricRingsStructurePlacement* placement) const {
    auto it = m_ringPositions.find(placement);
    if (it == m_ringPositions.end()) return nullptr;
    it->second->wait();   // Java: CompletableFuture.join() on first use
    if (std::getenv("OBEY_STRUCT_LOG")) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            uint64_t h = 1469598103934665603ull;
            for (const auto& pr : it->second->positions) {
                h ^= static_cast<uint64_t>(static_cast<uint32_t>(pr.first)); h *= 1099511628211ull;
                h ^= static_cast<uint64_t>(static_cast<uint32_t>(pr.second)); h *= 1099511628211ull;
            }
            std::fprintf(stderr, "[RingPositions] n=%zu hash=%016llx first=(%d,%d)\n",
                         it->second->positions.size(), static_cast<unsigned long long>(h),
                         it->second->positions.empty() ? 0 : it->second->positions[0].first,
                         it->second->positions.empty() ? 0 : it->second->positions[0].second);
        }
    }
    return &it->second->positions;
}

bool ChunkGeneratorStructureState::hasStructureChunkInRange(const std::string& structureSetName,
                                                            int32_t sourceX, int32_t sourceZ,
                                                            int32_t range) const {
    // Reference: hasStructureChunkInRange()
    const StructureSet& set = StructureSets::byName(structureSetName);
    for (int32_t testX = sourceX - range; testX <= sourceX + range; ++testX) {
        for (int32_t testZ = sourceZ - range; testZ <= sourceZ + range; ++testZ) {
            if (set.placement->isStructureChunk(*this, testX, testZ)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace structure
} // namespace levelgen
} // namespace minecraft
