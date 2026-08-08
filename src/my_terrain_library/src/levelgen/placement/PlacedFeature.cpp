#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/feature/Feature.h"
#include "levelgen/feature/BlockChangeTrace.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/WorldgenRandom.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
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

// Parity-debug: when MC_RNG_TRACE_FEATURE names this placed feature and
// MC_RNG_TRACE_FILE names a file, append every interface-level RNG draw and
// setBlock made during the configured feature's place() call. Mirrors the
// Java harness's per-feature RandomSource + WorldGenLevel wrappers so the two
// chronological streams diff line-by-line.
struct FeatureRngTraceScope {
    std::FILE* f = nullptr;
    // Env lookups are cached process-wide; the feature name (a demangle when
    // unnamed) is only materialized when tracing is actually configured.
    static bool configured() {
        static const bool v = [] {
            const char* want = std::getenv("MC_RNG_TRACE_FEATURE");
            const char* path = std::getenv("MC_RNG_TRACE_FILE");
            return want && path && *path;
        }();
        return v;
    }
    FeatureRngTraceScope(const PlacedFeature& placedFeature, const core::BlockPos& pos) {
        if (!configured()) {
            return;
        }
        const char* want = std::getenv("MC_RNG_TRACE_FEATURE");
        const char* path = std::getenv("MC_RNG_TRACE_FILE");
        if (placedFeature.getDebugName() == want) {
            f = std::fopen(path, "a");
            if (f) {
                std::fprintf(f, "RNGTRACE_BEGIN origin=%d,%d,%d\n",
                             pos.getX(), pos.getY(), pos.getZ());
                WorldgenRandom::s_rngTraceFile = f;
            }
        }
    }
    ~FeatureRngTraceScope() {
        if (f) {
            WorldgenRandom::s_rngTraceFile = nullptr;
            std::fprintf(f, "RNGTRACE_END\n");
            std::fclose(f);
        }
    }
};

// Parity-debug: when MC_TRACE_WATCH="x,y,z" is set (with MC_RNG_TRACE_FILE),
// log a WATCH line whenever the watched position's block changes across a
// configured feature's place() call. Mirrors the Java --trace-watch flag.
struct WatchPos {
    static const core::BlockPos* get() {
        static core::BlockPos pos(0, 0, 0);
        static bool parsed = false;
        static bool valid = false;
        if (!parsed) {
            parsed = true;
            if (const char* s = std::getenv("MC_TRACE_WATCH")) {
                int x, y, z;
                if (std::sscanf(s, "%d,%d,%d", &x, &y, &z) == 3) {
                    pos = core::BlockPos(x, y, z);
                    valid = true;
                }
            }
        }
        return valid ? &pos : nullptr;
    }

    static void report(const std::string& featureName, const core::BlockPos& origin,
                       BlockState* before, BlockState* after) {
        const core::BlockPos* wp = get();
        if (!wp || before == after) {
            return;
        }
        const char* path = std::getenv("MC_RNG_TRACE_FILE");
        if (!path || !*path) {
            return;
        }
        if (std::FILE* f = std::fopen(path, "a")) {
            std::fprintf(f, "WATCH %s origin=%d,%d,%d pos=%d,%d,%d %s -> %s\n",
                         featureName.c_str(),
                         origin.getX(), origin.getY(), origin.getZ(),
                         wp->getX(), wp->getY(), wp->getZ(),
                         before ? before->toStateString().c_str() : "null",
                         after ? after->toStateString().c_str() : "null");
            std::fclose(f);
        }
    }
};

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

// Reusable per-modifier position buffers for the placement pipeline. Leases
// nest — a configured feature can itself place other placed features
// mid-pipeline — so a deque-backed pool with a depth cursor keeps every live
// frame's buffer reference stable (deque push_back never invalidates) while
// buffers retain capacity across chunks.
thread_local std::deque<std::vector<core::BlockPos>> t_positionBufferPool;
thread_local size_t t_positionBufferDepth = 0;

struct PositionBufferLease {
    std::vector<core::BlockPos>& buffer;

    PositionBufferLease() : buffer(acquire()) {}
    ~PositionBufferLease() {
        buffer.clear();
        --t_positionBufferDepth;
    }
    PositionBufferLease(const PositionBufferLease&) = delete;
    PositionBufferLease& operator=(const PositionBufferLease&) = delete;

private:
    static std::vector<core::BlockPos>& acquire() {
        if (t_positionBufferDepth == t_positionBufferPool.size()) {
            t_positionBufferPool.emplace_back();
        }
        return t_positionBufferPool[t_positionBufferDepth++];
    }
};

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
    const bool loggingActive = s_loggingEnabled && s_logStream;
    const bool traceEnabled = loggingActive && s_modifierTracingEnabled;
    // getDebugName() demangles a type name when the feature has no explicit
    // name; only pay for it (and the BlockChangeTrace name bookkeeping) when
    // some debug consumer is actually on.
    const bool maintainName = loggingActive || feature::BlockChangeTrace::enabled;
    std::string featureName;
    std::string previousBlockTraceFeatureName;
    if (maintainName) {
        featureName = getDebugName();
        previousBlockTraceFeatureName = feature::BlockChangeTrace::currentFeatureName;
        feature::BlockChangeTrace::currentFeatureName = featureName;
    }

    if (!m_feature) {
        if (loggingActive) {
            *s_logStream << "STEP=" << s_currentStep << " IDX=" << s_currentIndex
                         << " " << featureName << " | placed=0 | null_feature=true\n";
        }
        if (maintainName) {
            feature::BlockChangeTrace::currentFeatureName = previousBlockTraceFeatureName;
        }
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

    auto processAndPlace = [&](auto&& self, const core::BlockPos& pos, size_t modifierIdx) -> void {
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

                {
                    BlockState* watchBefore = nullptr;
                    if (const core::BlockPos* wp = WatchPos::get()) {
                        watchBefore = level->getBlockState(*wp);
                    }
                    FeatureRngTraceScope rngScope(*this, pos);
                    traceCall.placed = m_feature->place(level, generator, random, pos);
                    if (const core::BlockPos* wp = WatchPos::get()) {
                        WatchPos::report(getDebugName(), pos, watchBefore, level->getBlockState(*wp));
                    }
                }

                feature::BlockChangeTrace::setCallback(previousCallback);
                traceCall.randomAfter = random.captureDebugState();
                placementTraceCalls.push_back(std::move(traceCall));

                if (placementTraceCalls.back().placed) {
                    placedAny = true;
                    ++placedCount;
                }
                return;
            }

            bool placedHere;
            {
                BlockState* watchBefore = nullptr;
                if (const core::BlockPos* wp = WatchPos::get()) {
                    watchBefore = level->getBlockState(*wp);
                }
                FeatureRngTraceScope rngScope(*this, pos);
                placedHere = m_feature->place(level, generator, random, pos);
                if (const core::BlockPos* wp = WatchPos::get()) {
                    WatchPos::report(getDebugName(), pos, watchBefore, level->getBlockState(*wp));
                }
            }
            if (placedHere) {
                placedAny = true;
                ++placedCount;
            }
            return;
        }

        PlacementModifier* modifier = m_placement[modifierIdx];
        if (!modifier) {
            return;
        }

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
            std::vector<core::BlockPos> newPositions = traceCall.outputPositions;
            actualModifierTraceCalls.push_back(std::move(traceCall));
            for (const core::BlockPos& newPos : newPositions) {
                self(self, newPos, modifierIdx + 1);
            }
            return;
        }

        PositionBufferLease lease;
        modifier->appendPositions(context, random, pos, lease.buffer);
        for (const core::BlockPos& newPos : lease.buffer) {
            self(self, newPos, modifierIdx + 1);
        }
    };

    processAndPlace(processAndPlace, origin, 0);

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
    } else if (loggingActive) {
        *s_logStream << "STEP=" << s_currentStep << " IDX=" << s_currentIndex
                     << " " << featureName
                     << " | placed=" << placedCount << "\n";
    }

    if (maintainName) {
        feature::BlockChangeTrace::currentFeatureName = previousBlockTraceFeatureName;
    }
    return placedAny;
}

} // namespace placement
} // namespace levelgen
} // namespace minecraft
