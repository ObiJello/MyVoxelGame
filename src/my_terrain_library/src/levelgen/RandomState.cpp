#include <string>
#include <cstdlib>
#include "levelgen/RandomState.h"
#include "levelgen/NoiseRouter.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/SurfaceSystem.h"
#include "world/biome/Climate.h"
#include "world/level/block/state/BlockState.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/DensityFunction.h"
#include "levelgen/DensityFunctions.h"
#include "synth/NormalNoise.h"
#include "synth/BlendedNoise.h"
#include "random/LegacyRandomSource.h"
#include "random/XoroshiroRandomSource.h"
#include "random/PositionalRandomFactory.h"
#include <unordered_map>

namespace minecraft {
namespace levelgen {

// Reference: RandomState.java lines 32-34
RandomState* RandomState::create(NoiseGeneratorSettings* settings, int64_t seed) {
    return new RandomState(settings, seed);
}

// Reference: RandomState.java lines 36-120
RandomState::RandomState(NoiseGeneratorSettings* settings, int64_t seed)
    : m_random(nullptr)
    , m_router(nullptr)
    , m_sampler(nullptr)
    , m_surfaceSystem(nullptr)
    , m_aquiferRandom(nullptr)
    , m_oreRandom(nullptr)
{
    // Reference: RandomState.java line 37
    // Get random source type from settings and create instance
    // For overworld: getRandomSource() returns XOROSHIRO, so we use XoroshiroRandomSource
    bool useLegacyInit = (settings->getRandomSource() == RandomAlgorithm::LEGACY);

    // Reference: RandomState.java lines 37-40:
    //   this.random = settings.getRandomSource().newInstance(seed).forkPositional();
    //   this.aquiferRandom = this.random.fromHashOf("minecraft:aquifer").forkPositional();
    //   this.oreRandom = this.random.fromHashOf("minecraft:ore").forkPositional();
    // legacy_random_source=true (nether/end) uses LegacyRandomSource end to
    // end; the overworld keeps the exact Xoroshiro objects as before.
    if (useLegacyInit) {
        LegacyRandomSource mainRandom(seed);
        m_random = new random::AnyPositionalRandomFactory(mainRandom.forkPositional());
        LegacyRandomSource aquiferSource = m_random->legacy().fromHashOf("minecraft:aquifer");
        m_aquiferRandom = new random::AnyPositionalRandomFactory(aquiferSource.forkPositional());
        LegacyRandomSource oreSource = m_random->legacy().fromHashOf("minecraft:ore");
        m_oreRandom = new random::AnyPositionalRandomFactory(oreSource.forkPositional());
    } else {
        XoroshiroRandomSource mainRandom(seed);
        m_random = new random::AnyPositionalRandomFactory(mainRandom.forkPositional());
        XoroshiroRandomSource aquiferRandomSource =
            m_random->xoroshiro().fromHashOf("minecraft:aquifer");
        m_aquiferRandom = new random::AnyPositionalRandomFactory(
            aquiferRandomSource.forkPositional());
        XoroshiroRandomSource oreRandomSource =
            m_random->xoroshiro().fromHashOf("minecraft:ore");
        m_oreRandom = new random::AnyPositionalRandomFactory(
            oreRandomSource.forkPositional());
    }

    // Reference: RandomState.java line 41
    // this.noiseIntances = new ConcurrentHashMap();
    // Already initialized as empty map in initializer list

    // Reference: RandomState.java line 42
    // this.positionalRandoms = new ConcurrentHashMap();
    // Already initialized as empty map in initializer list

    // Reference: RandomState.java lines 96-119
    // Wire up the router using NoiseWiringHelper visitor
    // This walks the density function tree and wires up noise holders with actual noise instances

    // Get the base router from settings
    NoiseRouter* baseRouter = settings->noiseRouter();

    // Create a noise wiring visitor that will replace keyed holders and
    // re-seed bootstrap-time unseeded functions the way Java RandomState does.
    class NoiseWiringVisitor : public density::DensityFunction::Visitor {
    public:
        NoiseWiringVisitor(RandomState* state, bool useLegacyInit, int64_t seed)
            : m_state(state)
            , m_useLegacyInit(useLegacyInit)
            , m_seed(seed) {}

        // Java's router is a DAG: registry Holders make every reference to
        // continents/erosion/ridges/offset/... the same object. The port's
        // NoiseRouterData expands each reference into its own copy, so this
        // wiring pass — the one that builds the router every NoiseChunk maps
        // from — memoises like NoiseChunk's WrapVisitor (DensityFunction::
        // mapAll's pre-key), producing the shared DAG once. Measured
        // 2026-08-30: NoiseChunks walked ~7,000 original nodes per chunk to
        // find their ~93 distinct ones. OBEY_NO_STRUCT_DEDUPE=1 turns it off
        // together with the NoiseChunk dedupe (A/B against the gen hash).
        bool memoises() const override {
            static const bool noDedupe = std::getenv("OBEY_NO_STRUCT_DEDUPE") != nullptr;
            return !noDedupe;
        }
        density::DensityFunction* lookupMapped(const density::DensityFunction* original) override {
            auto it = m_memo.find(original);
            return it == m_memo.end() ? nullptr : it->second;
        }
        void rememberMapped(const density::DensityFunction* original,
                            density::DensityFunction* mapped) override {
            m_memo.emplace(original, mapped);
            m_memo.emplace(mapped, mapped);
        }
        density::DensityFunction* lookupPreMapped(const std::string& key) override {
            auto it = m_preMap.find(key);
            return it == m_preMap.end() ? nullptr : it->second;
        }
        void rememberPreMapped(const std::string& key, density::DensityFunction* mapped) override {
            m_preMap.emplace(key, mapped);
        }
        std::unordered_map<const density::DensityFunction*, density::DensityFunction*> m_memo;
        std::unordered_map<std::string, density::DensityFunction*> m_preMap;

        density::DensityFunction* apply(density::DensityFunction* input) override {
            auto it = m_wrapped.find(input);
            if (it != m_wrapped.end()) {
                return it->second;
            }

            density::DensityFunction* wrapped = wrapNew(input);
            m_wrapped[input] = wrapped;
            return wrapped;
        }

        density::DensityFunction::NoiseHolder* visitNoise(density::DensityFunction::NoiseHolder* noiseHolder) override {
            if (!noiseHolder) {
                return nullptr;
            }

            const char* noiseName = noiseHolder->noiseName();
            if (noiseName == nullptr || noiseName[0] == '\0') {
                return noiseHolder;
            }

            // Reference: RandomState.java NoiseWiringHelper.visitNoise legacy
            // special cases (legacy_random_source=true, nether/end):
            // TEMPERATURE/VEGETATION use createLegacyNetherBiome with
            // LegacyRandomSource(seed+0/+1) and params(-7, 1.0, [1.0]);
            // SHIFT (minecraft:offset) becomes a ZERO noise
            // (params(0, 0.0, [])) from random.fromHashOf(offset).
            if (m_useLegacyInit) {
                std::string name(noiseName);
                if (name == "minecraft:temperature" || name == "temperature") {
                    LegacyRandomSource legacyRandom(m_seed + 0);
                    NormalNoise* noise = new NormalNoise(NormalNoise::createLegacyNetherBiome(
                        legacyRandom,
                        NormalNoise::NoiseParameters(-7, 1.0, std::vector<double>{1.0})));
                    return new density::DensityFunction::NoiseHolder(noise);
                }
                if (name == "minecraft:vegetation" || name == "vegetation") {
                    LegacyRandomSource legacyRandom(m_seed + 1);
                    NormalNoise* noise = new NormalNoise(NormalNoise::createLegacyNetherBiome(
                        legacyRandom,
                        NormalNoise::NoiseParameters(-7, 1.0, std::vector<double>{1.0})));
                    return new density::DensityFunction::NoiseHolder(noise);
                }
                if (name == "minecraft:offset" || name == "offset") {
                    LegacyRandomSource offsetRandom =
                        m_state->random()->legacy().fromHashOf("minecraft:offset");
                    NormalNoise* noise = new NormalNoise(NormalNoise::create(
                        offsetRandom,
                        NormalNoise::NoiseParameters(0, 0.0, std::vector<double>{})));
                    return new density::DensityFunction::NoiseHolder(noise);
                }
            }

            NormalNoise* noise = m_state->getOrCreateNoise(noiseName);
            return new density::DensityFunction::NoiseHolder(noise);
        }

    private:
        density::DensityFunction* wrapNew(density::DensityFunction* function) {
            if (!function) {
                return nullptr;
            }

            if (auto* noise = dynamic_cast<::minecraft::BlendedNoise*>(function)) {
                // Reference: NoiseWiringHelper.wrapNew - legacy uses
                // LegacyRandomSource(seed + 0); otherwise fromHashOf(terrain).
                if (m_useLegacyInit) {
                    LegacyRandomSource terrainRandom(m_seed + 0);
                    return new ::minecraft::BlendedNoise(noise->withNewRandom(terrainRandom));
                }
                XoroshiroRandomSource terrainRandom =
                    m_state->random()->xoroshiro().fromHashOf("minecraft:terrain");
                return new ::minecraft::BlendedNoise(noise->withNewRandom(terrainRandom));
            } else if (dynamic_cast<density::EndIslandDensityFunction*>(function)) {
                return new density::EndIslandDensityFunction(m_seed);
            }

            return function;
        }

        RandomState* m_state;
        bool m_useLegacyInit;
        int64_t m_seed;
        std::unordered_map<density::DensityFunction*, density::DensityFunction*> m_wrapped;
    };

    NoiseWiringVisitor wireVisitor(this, useLegacyInit, seed);

    // Wire up each density function in the router
    m_router = new NoiseRouter(
        baseRouter->barrierNoise()->mapAll(wireVisitor),
        baseRouter->fluidLevelFloodednessNoise()->mapAll(wireVisitor),
        baseRouter->fluidLevelSpreadNoise()->mapAll(wireVisitor),
        baseRouter->lavaNoise()->mapAll(wireVisitor),
        baseRouter->temperature()->mapAll(wireVisitor),
        baseRouter->vegetation()->mapAll(wireVisitor),
        baseRouter->continents()->mapAll(wireVisitor),
        baseRouter->erosion()->mapAll(wireVisitor),
        baseRouter->depth()->mapAll(wireVisitor),
        baseRouter->ridges()->mapAll(wireVisitor),
        baseRouter->preliminarySurfaceLevel()->mapAll(wireVisitor),
        baseRouter->finalDensity()->mapAll(wireVisitor),
        baseRouter->veinToggle()->mapAll(wireVisitor),
        baseRouter->veinRidged()->mapAll(wireVisitor),
        baseRouter->veinGap()->mapAll(wireVisitor)
    );

    // Reference: RandomState.java line 43
    // this.surfaceSystem = new SurfaceSystem(this, settings.defaultBlock(), settings.seaLevel(), this.random);
    m_surfaceSystem = new SurfaceSystem(this, settings->defaultBlock(), settings->seaLevel(), m_random);

    // Reference: RandomState.java line 119
    // Create climate sampler by flattening density functions
    // this.sampler = new Climate.Sampler(...)

    class NoiseFlattener : public density::DensityFunction::Visitor {
    public:
        density::DensityFunction* apply(density::DensityFunction* input) override {
            auto it = m_wrapped.find(input);
            if (it != m_wrapped.end()) {
                return it->second;
            }

            density::DensityFunction* wrapped = wrapNew(input);
            m_wrapped[input] = wrapped;
            return wrapped;
        }

    private:
        density::DensityFunction* wrapNew(density::DensityFunction* function) {
            if (auto* marker = dynamic_cast<density::DensityFunctions::MarkerOrMarked*>(function)) {
                return marker->wrapped();
            }
            return function;
        }

        std::unordered_map<density::DensityFunction*, density::DensityFunction*> m_wrapped;
    };

    NoiseFlattener noiseFlattener;

    std::vector<world::biome::Climate::ParameterPoint> spawnTarget;
    spawnTarget.reserve(settings->spawnTarget().size());
    for (ClimateParameterPoint* ptr : settings->spawnTarget()) {
        if (ptr) {
            spawnTarget.push_back(*reinterpret_cast<world::biome::Climate::ParameterPoint*>(ptr));
        }
    }

    // Reference: RandomState.java line 119
    // this.sampler = new Climate.Sampler(..., settings.spawnTarget())
    m_sampler = new world::biome::Climate::Sampler(
        m_router->temperature()->mapAll(noiseFlattener),
        m_router->vegetation()->mapAll(noiseFlattener),
        m_router->continents()->mapAll(noiseFlattener),
        m_router->erosion()->mapAll(noiseFlattener),
        m_router->depth()->mapAll(noiseFlattener),
        m_router->ridges()->mapAll(noiseFlattener),
        spawnTarget
    );
}

// Destructor
RandomState::~RandomState() {
    // Clean up positional random factories
    if (m_random) {
        delete m_random;
        m_random = nullptr;
    }
    if (m_aquiferRandom) {
        delete m_aquiferRandom;
        m_aquiferRandom = nullptr;
    }
    if (m_oreRandom) {
        delete m_oreRandom;
        m_oreRandom = nullptr;
    }

    // Clean up noise instances
    for (auto& pair : m_noiseInstances) {
        delete pair.second;
    }
    m_noiseInstances.clear();

    // Clean up additional positional randoms
    for (auto& pair : m_positionalRandoms) {
        delete pair.second;
    }
    m_positionalRandoms.clear();

    // Clean up sampler
    if (m_sampler) {
        delete m_sampler;
        m_sampler = nullptr;
    }

    // Clean up surface system
    if (m_surfaceSystem) {
        delete m_surfaceSystem;
        m_surfaceSystem = nullptr;
    }

    // NOTE: We do NOT delete m_router here because it's owned by NoiseGeneratorSettings
}

// Reference: RandomState.java lines 122-124
NormalNoise* RandomState::getOrCreateNoise(const std::string& noiseName) {
    // Java uses ConcurrentHashMap.computeIfAbsent here. Without synchronization,
    // concurrent SURFACE/NOISE reads can race and build duplicate instances.
    std::lock_guard<std::mutex> lock(m_noiseInstancesMutex);

    auto it = m_noiseInstances.find(noiseName);
    if (it != m_noiseInstances.end()) {
        return it->second;
    }

    // Reference: RandomState.java line 123
    // return (NormalNoise)this.noiseIntances.computeIfAbsent(noise, (key) -> Noises.instantiate(this.noises, this.random, noise));

    // Strip "minecraft:" prefix if present for registry lookup
    std::string baseName = noiseName;
    std::string fullIdentifier = noiseName;
    const std::string prefix = "minecraft:";
    if (noiseName.compare(0, prefix.size(), prefix) == 0) {
        baseName = noiseName.substr(prefix.size());
    } else {
        fullIdentifier = prefix + noiseName;
    }

    // Get noise parameters from registry using base name (without prefix)
    NoiseRegistry& registry = NoiseRegistry::instance();
    const NoiseRegistry::NoiseParameters& params = registry.getOrThrow(baseName);

    // Create random source for this noise
    // Reference: Noises.java line 78
    // NormalNoise.create(context.fromHashOf(((ResourceKey)holder.unwrapKey().orElseThrow()).identifier()), holder.value())
    // Java uses full ResourceKey identifier like "minecraft:temperature".
    // Legacy settings route through LegacyRandomSource (its NormalNoise
    // overload uses the legacy octave initialization).
    NormalNoise* noise;
    if (m_random->isLegacy()) {
        LegacyRandomSource noiseRandomSource = m_random->legacy().fromHashOf(fullIdentifier);
        noise = new NormalNoise(NormalNoise::create(noiseRandomSource, params.firstOctave, params.amplitudes));
    } else {
        XoroshiroRandomSource noiseRandomSource = m_random->xoroshiro().fromHashOf(fullIdentifier);
        noise = new NormalNoise(NormalNoise::create(noiseRandomSource, params.firstOctave, params.amplitudes));
    }

    // Store in map
    m_noiseInstances[noiseName] = noise;

    return noise;
}

// Reference: RandomState.java lines 126-128
random::AnyPositionalRandomFactory* RandomState::getOrCreateRandomFactory(const std::string& identifier) {
    // Java uses ConcurrentHashMap.computeIfAbsent here as well.
    std::lock_guard<std::mutex> lock(m_positionalRandomsMutex);

    auto it = m_positionalRandoms.find(identifier);
    if (it != m_positionalRandoms.end()) {
        return it->second;
    }

    // Reference: RandomState.java line 127
    // return (PositionalRandomFactory)this.positionalRandoms.computeIfAbsent(name, (key) -> this.random.fromHashOf(name).forkPositional());
    std::string fullIdentifier = identifier;
    if (fullIdentifier.rfind("minecraft:", 0) != 0) {
        fullIdentifier = "minecraft:" + fullIdentifier;
    }
    random::AnyPositionalRandomFactory* factory;
    if (m_random->isLegacy()) {
        LegacyRandomSource identRandomSource = m_random->legacy().fromHashOf(fullIdentifier);
        factory = new random::AnyPositionalRandomFactory(identRandomSource.forkPositional());
    } else {
        XoroshiroRandomSource identRandomSource = m_random->xoroshiro().fromHashOf(fullIdentifier);
        factory = new random::AnyPositionalRandomFactory(identRandomSource.forkPositional());
    }

    // Store in map
    m_positionalRandoms[identifier] = factory;

    return factory;
}

} // namespace levelgen
} // namespace minecraft
