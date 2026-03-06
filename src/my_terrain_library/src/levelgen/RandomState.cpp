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

    XoroshiroRandomSource mainRandom(seed);

    // Reference: RandomState.java line 37
    // this.random = settings.getRandomSource().newInstance(seed).forkPositional();
    m_random = new XoroshiroPositionalRandomFactory(mainRandom.forkPositional());

    // Reference: RandomState.java line 39
    // this.aquiferRandom = this.random.fromHashOf(Identifier.withDefaultNamespace("aquifer")).forkPositional();
    // CRITICAL: Java uses Identifier.withDefaultNamespace("aquifer").toString() = "minecraft:aquifer"
    XoroshiroRandomSource aquiferRandomSource = m_random->fromHashOf("minecraft:aquifer");
    m_aquiferRandom = new XoroshiroPositionalRandomFactory(aquiferRandomSource.forkPositional());

    // Reference: RandomState.java line 40
    // this.oreRandom = this.random.fromHashOf(Identifier.withDefaultNamespace("ore")).forkPositional();
    // CRITICAL: Java uses Identifier.withDefaultNamespace("ore").toString() = "minecraft:ore"
    XoroshiroRandomSource oreRandomSource = m_random->fromHashOf("minecraft:ore");
    m_oreRandom = new XoroshiroPositionalRandomFactory(oreRandomSource.forkPositional());

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

    // Create a noise wiring visitor that will replace NoiseHolder placeholders
    // with actual noise instances from this RandomState
    class NoiseWiringVisitor : public density::DensityFunction::Visitor {
    public:
        NoiseWiringVisitor(RandomState* state) : m_state(state) {}

        density::DensityFunction* apply(density::DensityFunction* input) override {
            // Pass through - we only transform NoiseHolders
            return input;
        }

        density::DensityFunction::NoiseHolder* visitNoise(density::DensityFunction::NoiseHolder* noiseHolder) override {
            // If the noise holder already has a noise instance, return it as-is
            if (noiseHolder->noise() != nullptr) {
                return noiseHolder;
            }

            // Get the noise name from the holder
            const char* noiseName = noiseHolder->noiseName();
            if (noiseName == nullptr || noiseName[0] == '\0') {
                // No name, return as-is
                return noiseHolder;
            }

            // Get or create the noise from RandomState
            NormalNoise* noise = m_state->getOrCreateNoise(noiseName);

            // Create a new NoiseHolder with the actual noise instance
            return new density::DensityFunction::NoiseHolder(noise);
        }

    private:
        RandomState* m_state;
    };

    NoiseWiringVisitor wireVisitor(this);

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

    // The Java code applies a "noiseFlattener" visitor that unwraps HolderHolder and Marker
    // For now, we'll use the density functions directly from the router
    density::DensityFunction* temperature = m_router->temperature();
    density::DensityFunction* vegetation = m_router->vegetation();
    density::DensityFunction* continents = m_router->continents();
    density::DensityFunction* erosion = m_router->erosion();
    density::DensityFunction* depth = m_router->depth();
    density::DensityFunction* ridges = m_router->ridges();

    // Reference: RandomState.java line 119
    // Create sampler (spawn target omitted for Phase 1)
    m_sampler = new world::biome::Climate::Sampler(
        temperature,
        vegetation,
        continents,
        erosion,
        depth,
        ridges
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
    // Check if already created
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
    // Java uses full ResourceKey identifier like "minecraft:temperature"
    XoroshiroRandomSource noiseRandomSource = m_random->fromHashOf(fullIdentifier);

    // Instantiate the noise
    NormalNoise* noise = new NormalNoise(NormalNoise::create(noiseRandomSource, params.firstOctave, params.amplitudes));

    // Store in map
    m_noiseInstances[noiseName] = noise;

    return noise;
}

// Reference: RandomState.java lines 126-128
random::PositionalRandomFactory* RandomState::getOrCreateRandomFactory(const std::string& identifier) {
    // Check if already created
    auto it = m_positionalRandoms.find(identifier);
    if (it != m_positionalRandoms.end()) {
        return it->second;
    }

    // Reference: RandomState.java line 127
    // return (PositionalRandomFactory)this.positionalRandoms.computeIfAbsent(name, (key) -> this.random.fromHashOf(name).forkPositional());
    // Java uses full ResourceKey identifier like "minecraft:ore"
    std::string fullIdentifier = "minecraft:" + identifier;
    XoroshiroRandomSource identRandomSource = m_random->fromHashOf(fullIdentifier);
    random::PositionalRandomFactory* factory = new random::PositionalRandomFactory(identRandomSource.forkPositional());

    // Store in map
    m_positionalRandoms[identifier] = factory;

    return factory;
}

} // namespace levelgen
} // namespace minecraft
