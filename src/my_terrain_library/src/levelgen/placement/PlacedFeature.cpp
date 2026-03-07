#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/feature/Feature.h"
#include "levelgen/feature/BlockChangeTrace.h"
#include "levelgen/ChunkGenerator.h"
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <typeinfo>
#include <utility>
#include <vector>
#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#endif

// Reference: net/minecraft/world/level/levelgen/placement/PlacedFeature.java

// Feature logging state
static bool s_loggingEnabled = false;
static std::ostream* s_logStream = &std::cerr;
static thread_local int s_currentStep = -1;
static thread_local int s_currentIndex = -1;

// Detailed logging mode: 0=basic, 1=positions, 2=verbose
static int s_detailLevel = 0;

// Modifier tracing mode: when true, logs the real lazy placement path
static bool s_modifierTracingEnabled = false;

namespace minecraft {
namespace levelgen {
namespace placement {
namespace {

struct ModifierTraceCall {
    size_t callIndex = 0;
    size_t modifierIndex = 0;
    std::string typeName;
    core::BlockPos inputPos;
    std::vector<core::BlockPos> outputPositions;
    std::string detail;
    WorldgenRandom::DebugStateSnapshot randomBefore{};
    WorldgenRandom::DebugStateSnapshot randomAfter{};
};

struct PlacementTraceCall {
    size_t callIndex = 0;
    core::BlockPos position;
    bool placed = false;
    std::vector<feature::BlockChangeEvent> blockChanges;
    WorldgenRandom::DebugStateSnapshot randomBefore{};
    WorldgenRandom::DebugStateSnapshot randomAfter{};
};

std::string demangleTypeName(const char* mangledName) {
    if (!mangledName) {
        return "(unnamed)";
    }

#if __has_include(<cxxabi.h>)
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangledName, nullptr, nullptr, &status);
    if (status == 0 && demangled) {
        std::string result(demangled);
        std::free(demangled);
        return result;
    }
    if (demangled) {
        std::free(demangled);
    }
#endif

    return mangledName;
}

std::string formatPos(const core::BlockPos& pos) {
    return std::to_string(pos.getX()) + "," +
           std::to_string(pos.getY()) + "," +
           std::to_string(pos.getZ());
}

std::string formatRandomState(const WorldgenRandom::DebugStateSnapshot& state) {
    std::ostringstream out;
    out << "seed_lo=" << state.seedLo
        << " seed_hi=" << state.seedHi
        << " count=" << state.count
        << " gauss_cached=" << (state.haveNextNextGaussian ? "true" : "false");
    if (state.haveNextNextGaussian) {
        out << " gauss_value=" << std::setprecision(17) << state.nextNextGaussian;
    }
    return out.str();
}

} // namespace

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

std::string PlacedFeature::getDebugName() const {
    if (!m_name.empty()) {
        return m_name;
    }
    if (m_feature) {
        return demangleTypeName(typeid(*m_feature).name());
    }
    return "(unnamed)";
}

bool PlacedFeature::place(
    WorldGenLevel* level,
    ChunkGenerator* generator,
    WorldgenRandom& random,
    const core::BlockPos& origin
) {
    PlacementContext context(level, generator, std::nullopt);
    return placeWithContext(context, random, origin);
}

bool PlacedFeature::placeWithBiomeCheck(
    WorldGenLevel* level,
    ChunkGenerator* generator,
    WorldgenRandom& random,
    const core::BlockPos& origin
) {
    PlacementContext context(level, generator, std::optional<const PlacedFeature*>(this));
    return placeWithContext(context, random, origin);
}

bool PlacedFeature::placeWithContext(
    PlacementContext& context,
    WorldgenRandom& random,
    const core::BlockPos& origin
) {
    const std::string featureName = getDebugName();
    const bool traceEnabled = s_loggingEnabled && s_logStream && s_modifierTracingEnabled;
    const std::string previousBlockTraceFeatureName = feature::BlockChangeTrace::currentFeatureName;
    feature::BlockChangeTrace::currentFeatureName = featureName;

    if (!m_feature) {
        if (s_loggingEnabled && s_logStream) {
            *s_logStream << "STEP=" << s_currentStep << " IDX=" << s_currentIndex
                         << " " << featureName << " | placed=0 | null_feature=true\n";
        }
        feature::BlockChangeTrace::currentFeatureName = previousBlockTraceFeatureName;
        return false;
    }

    bool placedAny = false;
    int placedCount = 0;
    WorldGenLevel* level = context.getLevel();
    ChunkGenerator* generator = context.generator();

    std::vector<ModifierTraceCall> modifierTraceCalls;
    std::vector<PlacementTraceCall> placementTraceCalls;
    size_t modifierCallIndex = 0;
    size_t placementCallIndex = 0;

    if (s_loggingEnabled && s_logStream && s_detailLevel >= 2 && !traceEnabled) {
        *s_logStream << "  MODIFIERS=" << m_placement.size() << "\n";
        for (size_t i = 0; i < m_placement.size(); ++i) {
            PlacementModifier* modifier = m_placement[i];
            *s_logStream << "    [" << i << "] "
                         << (modifier ? modifier->getTypeName() : "null")
                         << "\n";
        }
    }

    std::function<void(const core::BlockPos&, size_t)> processAndPlace;
    processAndPlace = [&](const core::BlockPos& pos, size_t modifierIdx) {
        if (modifierIdx >= m_placement.size()) {
            if (traceEnabled) {
                PlacementTraceCall traceCall;
                traceCall.callIndex = placementCallIndex++;
                traceCall.position = pos;
                traceCall.randomBefore = random.captureDebugState();

                auto previousCallback = feature::BlockChangeTrace::callback;
                feature::BlockChangeTrace::setCallback([&traceCall](const feature::BlockChangeEvent& event) {
                    traceCall.blockChanges.push_back(event);
                });

                traceCall.placed = m_feature->place(level, generator, random, pos);

                feature::BlockChangeTrace::setCallback(previousCallback);
                traceCall.randomAfter = random.captureDebugState();
                placementTraceCalls.push_back(std::move(traceCall));

                if (placementTraceCalls.back().placed) {
                    placedAny = true;
                    ++placedCount;
                }
                return;
            }

            if (m_feature->place(level, generator, random, pos)) {
                placedAny = true;
                ++placedCount;
            }
            return;
        }

        PlacementModifier* modifier = m_placement[modifierIdx];
        if (!modifier) {
            return;
        }

        WorldgenRandom::DebugStateSnapshot before{};
        WorldgenRandom::DebugStateSnapshot after{};
        if (traceEnabled) {
            before = random.captureDebugState();
        }

        std::vector<core::BlockPos> newPositions = modifier->getPositions(context, random, pos);

        if (traceEnabled) {
            after = random.captureDebugState();
            ModifierTraceCall traceCall;
            traceCall.callIndex = modifierCallIndex++;
            traceCall.modifierIndex = modifierIdx;
            traceCall.typeName = modifier->getTypeName();
            traceCall.inputPos = pos;
            traceCall.outputPositions = newPositions;
            traceCall.detail = modifier->describeTrace(context, pos, newPositions);
            traceCall.randomBefore = before;
            traceCall.randomAfter = after;
            modifierTraceCalls.push_back(std::move(traceCall));
        }

        for (const core::BlockPos& newPos : newPositions) {
            processAndPlace(newPos, modifierIdx + 1);
        }
    };

    processAndPlace(origin, 0);

    if (traceEnabled) {
        *s_logStream << "FEATURE STEP=" << s_currentStep << " IDX=" << s_currentIndex
                     << " " << featureName << "\n";
        *s_logStream << "  ORIGIN=" << formatPos(origin) << "\n";
        *s_logStream << "  MODIFIER_COUNT=" << m_placement.size() << "\n";

        std::string modifierChain;
        for (size_t i = 0; i < m_placement.size(); ++i) {
            PlacementModifier* modifier = m_placement[i];
            if (!modifierChain.empty()) {
                modifierChain += ",";
            }
            modifierChain += modifier ? modifier->getTypeName() : "null";
        }
        *s_logStream << "  MODIFIER_CHAIN=" << modifierChain << "\n";

        for (const ModifierTraceCall& traceCall : modifierTraceCalls) {
            *s_logStream << "  MOD_CALL[" << traceCall.callIndex << "]"
                         << " idx=" << traceCall.modifierIndex
                         << " type=" << traceCall.typeName
                         << " input=" << formatPos(traceCall.inputPos)
                         << " out_count=" << traceCall.outputPositions.size()
                         << " " << formatRandomState(traceCall.randomBefore)
                         << " -> " << formatRandomState(traceCall.randomAfter)
                         << "\n";
            if (!traceCall.detail.empty()) {
                *s_logStream << "    DETAIL=" << traceCall.detail << "\n";
            }
            for (size_t i = 0; i < traceCall.outputPositions.size(); ++i) {
                *s_logStream << "    OUT[" << i << "]=" << formatPos(traceCall.outputPositions[i]) << "\n";
            }
        }

        for (const PlacementTraceCall& traceCall : placementTraceCalls) {
            *s_logStream << "  PLACE[" << traceCall.callIndex << "]"
                         << " pos=" << formatPos(traceCall.position)
                         << " placed=" << (traceCall.placed ? "true" : "false")
                         << " block_changes=" << traceCall.blockChanges.size()
                         << " " << formatRandomState(traceCall.randomBefore)
                         << " -> " << formatRandomState(traceCall.randomAfter)
                         << "\n";
            for (size_t i = 0; i < traceCall.blockChanges.size(); ++i) {
                const feature::BlockChangeEvent& change = traceCall.blockChanges[i];
                *s_logStream << "    BLOCK[" << i << "]="
                             << change.x << "," << change.y << "," << change.z
                             << " old=" << change.oldBlock
                             << " new=" << change.newBlock << "\n";
            }
        }

        *s_logStream << "  RESULT: placed=" << placedCount << "\n";
    } else if (s_loggingEnabled && s_logStream) {
        *s_logStream << "STEP=" << s_currentStep << " IDX=" << s_currentIndex
                     << " " << featureName
                     << " | placed=" << placedCount << "\n";
    }

    feature::BlockChangeTrace::currentFeatureName = previousBlockTraceFeatureName;
    return placedAny;
}

} // namespace placement
} // namespace levelgen
} // namespace minecraft
