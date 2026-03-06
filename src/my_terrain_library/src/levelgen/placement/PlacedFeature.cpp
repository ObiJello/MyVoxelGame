#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/feature/Feature.h"
#include "levelgen/ChunkGenerator.h"
#include <iostream>
#include <iomanip>

// Reference: net/minecraft/world/level/levelgen/placement/PlacedFeature.java

// Debug flag - set to true to trace tree placement
static bool s_debugTreePlacement = false;

// Feature logging state
static bool s_loggingEnabled = false;
static std::ostream* s_logStream = &std::cerr;
static int s_currentStep = -1;
static int s_currentIndex = -1;

// Detailed logging mode: 0=basic, 1=positions, 2=verbose (all modifiers)
static int s_detailLevel = 0;

// Modifier tracing mode: when true, logs per-modifier position transformations
static bool s_modifierTracingEnabled = false;

namespace minecraft {
namespace levelgen {
namespace placement {

//=============================================================================
// Static Logging Methods
//=============================================================================

void PlacedFeature::setLoggingEnabled(bool enabled) {
    s_loggingEnabled = enabled;
}

void PlacedFeature::setDetailLevel(int level) {
    s_detailLevel = level;
}

int PlacedFeature::getDetailLevel() {
    return s_detailLevel;
}

bool PlacedFeature::isLoggingEnabled() {
    return s_loggingEnabled;
}

void PlacedFeature::setLogStream(std::ostream* stream) {
    s_logStream = stream ? stream : &std::cerr;
}

std::ostream* PlacedFeature::getLogStream() {
    return s_logStream;
}

void PlacedFeature::setCurrentStepIndex(int step, int index) {
    s_currentStep = step;
    s_currentIndex = index;
}

int PlacedFeature::getCurrentStep() {
    return s_currentStep;
}

int PlacedFeature::getCurrentIndex() {
    return s_currentIndex;
}

void PlacedFeature::setModifierTracingEnabled(bool enabled) {
    s_modifierTracingEnabled = enabled;
}

bool PlacedFeature::isModifierTracingEnabled() {
    return s_modifierTracingEnabled;
}

bool PlacedFeature::place(
    WorldGenLevel* level,
    ChunkGenerator* generator,
    WorldgenRandom& random,
    const core::BlockPos& origin
) {
    // Reference: PlacedFeature.java lines 28-30
    // return this.placeWithContext(new PlacementContext(level, generator, Optional.empty()), random, origin);
    PlacementContext context(level, generator, std::nullopt);
    return placeWithContext(context, random, origin);
}

bool PlacedFeature::placeWithBiomeCheck(
    WorldGenLevel* level,
    ChunkGenerator* generator,
    WorldgenRandom& random,
    const core::BlockPos& origin
) {
    // Reference: PlacedFeature.java lines 32-34
    // return this.placeWithContext(new PlacementContext(level, generator, Optional.of(this)), random, origin);
    PlacementContext context(level, generator, std::optional<const PlacedFeature*>(this));
    return placeWithContext(context, random, origin);
}

bool PlacedFeature::placeWithContext(
    PlacementContext& context,
    WorldgenRandom& random,
    const core::BlockPos& origin
) {
    // Output STEP header FIRST so trace data appears AFTER the header
    // (This makes trace output easier to read - trace belongs to the feature above it)
    std::string featureName = m_name.empty() ? "(unnamed)" : m_name;
    if (s_loggingEnabled && s_logStream && s_modifierTracingEnabled) {
        *s_logStream << "FEATURE STEP=" << s_currentStep << " IDX=" << s_currentIndex
                     << " " << featureName << "\n";
    }

    // Reference: PlacedFeature.java lines 36-55
    // Stream<BlockPos> placements = Stream.of(origin);
    // for(PlacementModifier placementModifier : this.placement) {
    //     placements = placements.flatMap((p) -> placementModifier.getPositions(context, random, p));
    // }
    //
    // CRITICAL: Java streams are lazily evaluated! This means each position goes through
    // ALL modifiers before the next position is processed. The random calls must interleave
    // per-position, not per-modifier.

    // =========================================================================
    // MODIFIER TRACING PASS (if enabled)
    // Clone state, trace, then restore for actual placement
    // =========================================================================
    if (s_modifierTracingEnabled && s_loggingEnabled && s_logStream) {
        // Save random state
        uint64_t savedSeedLo, savedSeedHi;
        random.getSeedState(savedSeedLo, savedSeedHi);

        // =====================================================================
        // DIFFABLE OUTPUT FORMAT - designed for easy Java/C++ comparison
        // =====================================================================

        // Line 1: Feature identification with modifier count
        *s_logStream << "  MODIFIER_COUNT=" << m_placement.size() << "\n";

        // Line 2: Complete modifier chain as single diffable string
        // Format: MODIFIER_CHAIN=Type1,Type2,Type3,...
        std::string modifierChain;
        for (size_t i = 0; i < m_placement.size(); i++) {
            if (m_placement[i]) {
                if (!modifierChain.empty()) modifierChain += ",";
                modifierChain += m_placement[i]->getTypeName();
            }
        }
        *s_logStream << "  MODIFIER_CHAIN=" << modifierChain << "\n";

        // Line 3: Seed state before any modifiers run
        *s_logStream << "  SEED_BEFORE=" << savedSeedLo << "," << savedSeedHi << "\n";

        // Track positions through each modifier level
        // We process breadth-first per modifier to show modifier-level transformations
        std::vector<core::BlockPos> currentLevelPositions = {origin};

        for (size_t modIdx = 0; modIdx < m_placement.size(); modIdx++) {
            PlacementModifier* modifier = m_placement[modIdx];
            if (!modifier) continue;

            // Capture seed state BEFORE this modifier runs
            uint64_t beforeLo, beforeHi;
            random.getSeedState(beforeLo, beforeHi);

            std::vector<core::BlockPos> nextLevelPositions;
            std::vector<std::pair<core::BlockPos, core::BlockPos>> transformations; // input -> output pairs

            // Process each position through this modifier
            for (const auto& pos : currentLevelPositions) {
                auto results = modifier->getPositions(context, random, pos);
                for (const auto& result : results) {
                    nextLevelPositions.push_back(result);
                    if (transformations.size() < 10) { // Store first 10 for logging
                        transformations.push_back({pos, result});
                    }
                }
            }

            // Capture seed state AFTER this modifier runs
            uint64_t afterLo, afterHi;
            random.getSeedState(afterLo, afterHi);

            // Log this modifier with diffable format:
            // MOD[idx]=TypeName in=N out=M seed_before=lo,hi seed_after=lo,hi
            *s_logStream << "  MOD[" << modIdx << "]=" << modifier->getTypeName()
                         << " in=" << currentLevelPositions.size()
                         << " out=" << nextLevelPositions.size()
                         << " seed_before=" << beforeLo << "," << beforeHi
                         << " seed_after=" << afterLo << "," << afterHi << "\n";
            s_logStream->flush();

            // Show first few position transformations for debugging
            for (size_t i = 0; i < transformations.size() && i < 5; i++) {
                const auto& [inPos, outPos] = transformations[i];
                *s_logStream << "    POS[" << i << "]: "
                             << inPos.getX() << "," << inPos.getY() << "," << inPos.getZ()
                             << " -> "
                             << outPos.getX() << "," << outPos.getY() << "," << outPos.getZ() << "\n";
            }
            if (nextLevelPositions.size() > 5) {
                *s_logStream << "    ... and " << (nextLevelPositions.size() - 5) << " more positions\n";
            }
            s_logStream->flush();

            currentLevelPositions = std::move(nextLevelPositions);
            if (currentLevelPositions.empty()) break;
        }

        // Final position count after all modifiers
        *s_logStream << "  FINAL_POSITIONS=" << currentLevelPositions.size() << "\n";

        // RESTORE random state for actual placement
        random.setSeedState(savedSeedLo, savedSeedHi);
    }

    // =========================================================================
    // ACTUAL PLACEMENT PASS (using correct lazy stream semantics)
    // =========================================================================

    // Log modifier list if verbose logging enabled (but not tracing)
    if (s_loggingEnabled && s_logStream && s_detailLevel >= 2 && !s_modifierTracingEnabled) {
        *s_logStream << "  MODIFIERS=" << m_placement.size() << "\n";
        for (size_t i = 0; i < m_placement.size(); i++) {
            auto* mod = m_placement[i];
            std::string modName = mod ? mod->getTypeName() : "null";
            *s_logStream << "    [" << i << "] " << modName << "\n";
        }
    }

    // CRITICAL FIX: Feature placement must happen INSIDE the lazy stream,
    // not after collecting all positions. In Java, Stream.forEach() processes
    // each element through ALL flatMap stages AND the terminal forEach action
    // before moving to the next element. This means feature.place() for position 1
    // (which consumes many random calls) happens BEFORE position 2 even enters
    // the InSquarePlacement modifier.
    //
    // Reference: PlacedFeature.java lines 36-54
    //   Stream<BlockPos> placements = Stream.of(origin);
    //   for(PlacementModifier mod : this.placement) {
    //       placements = placements.flatMap((p) -> mod.getPositions(context, random, p));
    //   }
    //   placements.forEach((pos) -> { feature.place(..., random, pos); });

    bool placedAny = false;
    if (!m_feature) return false;

    WorldGenLevel* level = context.getLevel();
    ChunkGenerator* generator = context.generator();
    int placedCount = 0;

    // Recursive helper that processes positions through modifiers AND places them
    // This correctly simulates Java's lazy stream: each position goes through
    // all modifiers and gets placed before the next position starts.
    // Debug: track BiomeFilter results for dripstone features
    bool debugDripstone = s_loggingEnabled && s_logStream &&
        (m_name.find("DRIPSTONE") != std::string::npos || m_name.find("dripstone") != std::string::npos);


    int biomePassCount = 0, biomeFailCount = 0;

    std::function<void(const core::BlockPos&, size_t)> processAndPlace;
    processAndPlace = [&](const core::BlockPos& pos, size_t modifierIdx) {
        if (modifierIdx >= m_placement.size()) {
            // Reached end of modifiers - PLACE the feature here (inside the stream)
            if (m_feature->place(level, generator, random, pos)) {
                placedAny = true;
                placedCount++;
            }
            return;
        }

        PlacementModifier* modifier = m_placement[modifierIdx];
        if (!modifier) return;

        // Get positions from this modifier
        auto newPositions = modifier->getPositions(context, random, pos);

        // Debug: log BiomeFilter pass/fail for dripstone
        if (debugDripstone && modifier->getTypeName() == "BiomeFilter") {
            if (!newPositions.empty()) {
                biomePassCount++;
                const world::biome::Biome* biome = context.getBiome(pos);
                if (biome && biomePassCount <= 5) {
                    *s_logStream << "  BIOME_PASS: (" << pos.getX() << "," << pos.getY() << "," << pos.getZ()
                                 << ") biome=" << biome->getName() << "\n";
                }
            } else {
                biomeFailCount++;
            }
        }

        // Recursively process each resulting position through remaining modifiers
        for (const auto& newPos : newPositions) {
            processAndPlace(newPos, modifierIdx + 1);
        }
    };

    // Process origin through all modifiers with inline placement
    processAndPlace(origin, 0);


    // Log dripstone BiomeFilter summary
    if (debugDripstone && s_logStream) {
        *s_logStream << "  BIOME_SUMMARY: pass=" << biomePassCount << " fail=" << biomeFailCount
                     << " total=" << (biomePassCount + biomeFailCount) << "\n";
    }

    // Log feature placement if logging is enabled
    if (s_loggingEnabled && s_logStream) {
        if (s_modifierTracingEnabled) {
            *s_logStream << "  RESULT: placed=" << placedCount << "\n";
        } else {
            *s_logStream << "STEP=" << s_currentStep << " IDX=" << s_currentIndex
                         << " " << featureName
                         << " | placed=" << placedCount << "\n";
        }
    }

    return placedAny;
}

} // namespace placement
} // namespace levelgen
} // namespace minecraft
