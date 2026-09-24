#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/terrain/Aquifer.h"

#include "external/json.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Reference: levelgen.NoiseRouter, NoiseSettings and NoiseGeneratorSettings
// (26.3), decoded from worldgen/noise_settings/<name>.json through the
// density function codec (WorldgenRegistries). The material rule stays a
// registry name; the material system resolves it.

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {
class BlockState;
}
} // namespace block
} // namespace level
} // namespace world

namespace levelgen {
namespace density {

class WorldgenRegistries;

// NoiseRouter (record).
struct NoiseRouter {
    DensityFunctionPtr temperature;
    DensityFunctionPtr vegetation;
    DensityFunctionPtr continents;
    DensityFunctionPtr erosion;
    DensityFunctionPtr depth;
    DensityFunctionPtr ridges;
    DensityFunctionPtr chunkSurfaceLevel;
    DensityFunctionPtr finalDensity;

    static NoiseRouter fromJson(const nlohmann::json& json, WorldgenRegistries& registries);
};

// NoiseSettings (record).
struct NoiseSettings {
    int minY = 0;
    int height = 0;

    // Throws std::runtime_error when guardY rejects the values.
    static NoiseSettings create(int minY, int height);
    // NoiseSettings.clampToHeightAccessor.
    NoiseSettings clampToHeightAccessor(int levelMinY, int levelMaxY) const;
};

// SpawnTargetPoint: registry density function -> Climate.Parameter span.
struct SpawnTargetPoint {
    struct Entry {
        DensityFunctionPtr function;   // a registry reference
        float min;
        float max;
    };
    std::vector<Entry> parameters;
};

// NoiseGeneratorSettings.DebugFunctionEntry.
struct DebugFunctionEntry {
    std::string label;
    DensityFunctionPtr function;
};

class TerrainSettings {
public:
    using BlockState = ::minecraft::world::level::block::state::BlockState;

    NoiseSettings noiseSettings;
    BlockState* defaultBlock = nullptr;
    BlockState* defaultFluid = nullptr;
    NoiseRouter noiseRouter;
    std::string materialRule;   // Holder<MaterialRule>: a registry name
    std::vector<SpawnTargetPoint> spawnTarget;
    int seaLevel = 0;
    bool disableMobGeneration = false;
    std::optional<Aquifer::Config> aquifers;
    bool useLegacyRandomSource = false;
    std::vector<DebugFunctionEntry> debugFunctions;

    // NoiseGeneratorSettings.DIRECT_CODEC.
    static std::shared_ptr<const TerrainSettings> fromJson(const nlohmann::json& json, WorldgenRegistries& registries);
    // Registries.NOISE_SETTINGS entry "minecraft:overworld" etc.
    static std::shared_ptr<const TerrainSettings> load(const std::string& key, WorldgenRegistries& registries);

    // BlockState.CODEC: "minecraft:stone", "minecraft:x[a=b]" or {Name, Properties}.
    static BlockState* parseBlockState(const nlohmann::json& json);
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
