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

// Modifier tracing mode: when true, logs batch-style modifier expansion for
// easier comparison with the Java async trace, while preserving the real lazy
// placement semantics for generation itself.
static bool s_modifierTracingEnabled = false;

namespace minecraft {
namespace levelgen {
namespace placement {
namespace {

struct BatchModifierTraceTransform {
    core::BlockPos inputPos;
    std::vector<core::BlockPos> outputPositions;
    std::string detail;
};

struct BatchModifierTraceCall {
    size_t callIndex = 0;
    size_t modifierIndex = 0;
    std::string typeName;
    std::vector<core::BlockPos> inputPositions;
    std::vector<BatchModifierTraceTransform> transforms;
    std::vector<core::BlockPos> outputPositions;
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

struct ActualModifierTraceCall {
    size_t callIndex = 0;
    size_t modifierIndex = 0;
    core::BlockPos inputPos;
    std::string typeName;
    std::vector<core::BlockPos> outputPositions;
    std::string detail;
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

    std::vector<BatchModifierTraceCall> modifierTraceCalls;
    std::vector<core::BlockPos> batchPlacementPositions;
    std::vector<ActualModifierTraceCall> actualModifierTraceCalls;
    std::vector<PlacementTraceCall> placementTraceCalls;
    size_t actualModifierCallIndex = 0;
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

    if (traceEnabled) {
        const WorldgenRandom::DebugStateSnapshot traceStartState = random.captureDebugState();
        std::vector<core::BlockPos> currentPositions{origin};

        for (size_t modifierIdx = 0; modifierIdx < m_placement.size(); ++modifierIdx) {
            BatchModifierTraceCall traceCall;
            traceCall.callIndex = modifierTraceCalls.size();
            traceCall.modifierIndex = modifierIdx;
            traceCall.inputPositions = currentPositions;
            traceCall.randomBefore = random.captureDebugState();

            PlacementModifier* modifier = m_placement[modifierIdx];
            traceCall.typeName = modifier ? modifier->getTypeName() : "null";

            if (modifier) {
                std::vector<core::BlockPos> nextPositions;
                for (const core::BlockPos& inputPos : currentPositions) {
                    BatchModifierTraceTransform transform;
                    transform.inputPos = inputPos;
                    transform.outputPositions = modifier->getPositions(context, random, inputPos);
                    transform.detail = modifier->describeTrace(context, inputPos, transform.outputPositions);

                    nextPositions.insert(
                        nextPositions.end(),
                        transform.outputPositions.begin(),
                        transform.outputPositions.end()
                    );
                    traceCall.transforms.push_back(std::move(transform));
                }
                traceCall.outputPositions = std::move(nextPositions);
                currentPositions = traceCall.outputPositions;
            } else {
                currentPositions.clear();
            }

            traceCall.randomAfter = random.captureDebugState();
            modifierTraceCalls.push_back(std::move(traceCall));

            if (currentPositions.empty()) {
                break;
            }
        }

        batchPlacementPositions = currentPositions;
        random.restoreDebugState(traceStartState);
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

        std::vector<core::BlockPos> newPositions;
        if (traceEnabled) {
            ActualModifierTraceCall traceCall;
            traceCall.callIndex = actualModifierCallIndex++;
            traceCall.modifierIndex = modifierIdx;
            traceCall.inputPos = pos;
            traceCall.typeName = modifier->getTypeName();
            traceCall.randomBefore = random.captureDebugState();
            traceCall.outputPositions = modifier->getPositions(context, random, pos);
            traceCall.detail = modifier->describeTrace(context, pos, traceCall.outputPositions);
            traceCall.randomAfter = random.captureDebugState();
            newPositions = traceCall.outputPositions;
            actualModifierTraceCalls.push_back(std::move(traceCall));
        } else {
            newPositions = modifier->getPositions(context, random, pos);
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

        *s_logStream << "  TRACE_STYLE=batch_modifiers+actual_recursive_calls+actual_placements\n";

        for (const BatchModifierTraceCall& traceCall : modifierTraceCalls) {
            *s_logStream << "  BATCH_MOD_CALL[" << traceCall.callIndex << "]"
                         << " idx=" << traceCall.modifierIndex
                         << " type=" << traceCall.typeName
                         << " input_count=" << traceCall.inputPositions.size()
                         << " out_count=" << traceCall.outputPositions.size()
                         << " " << formatRandomState(traceCall.randomBefore)
                         << " -> " << formatRandomState(traceCall.randomAfter)
                         << "\n";
            for (size_t transformIdx = 0; transformIdx < traceCall.transforms.size(); ++transformIdx) {
                const BatchModifierTraceTransform& transform = traceCall.transforms[transformIdx];
                *s_logStream << "    INPUT[" << transformIdx << "]="
                             << formatPos(transform.inputPos)
                             << " out_count=" << transform.outputPositions.size()
                             << "\n";
                if (!transform.detail.empty()) {
                    *s_logStream << "      DETAIL=" << transform.detail << "\n";
                }
                for (size_t i = 0; i < transform.outputPositions.size(); ++i) {
                    *s_logStream << "      OUT[" << i << "]="
                                 << formatPos(transform.outputPositions[i]) << "\n";
                }
            }
        }

        *s_logStream << "  BATCH_TRACE_FINAL=" << batchPlacementPositions.size() << "\n";
        for (size_t i = 0; i < batchPlacementPositions.size(); ++i) {
            *s_logStream << "    BATCH_PLACE[" << i << "]="
                         << formatPos(batchPlacementPositions[i]) << "\n";
        }

        for (const ActualModifierTraceCall& traceCall : actualModifierTraceCalls) {
            *s_logStream << "  ACTUAL_MOD_CALL[" << traceCall.callIndex << "]"
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
                *s_logStream << "    OUT[" << i << "]="
                             << formatPos(traceCall.outputPositions[i]) << "\n";
            }
        }

        for (const PlacementTraceCall& traceCall : placementTraceCalls) {
            *s_logStream << "  ACTUAL_PLACE[" << traceCall.callIndex << "]"
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
