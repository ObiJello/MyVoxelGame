#include "levelgen/density/terrain/TerrainSettings.h"

#include "levelgen/density/WorldgenRegistries.h"
#include "world/level/block/Blocks.h"

#include <algorithm>
#include <map>
#include <stdexcept>

// Reference: NoiseRouter.CODEC, NoiseSettings.CODEC, NoiseGeneratorSettings.
// DIRECT_CODEC, SpawnTargetPoint.CODEC, Aquifer.Config.CODEC (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

using nlohmann::json;

namespace {

// DimensionType.MIN_Y / MAX_Y / Y_SIZE.
constexpr int MIN_Y = -2032;
constexpr int MAX_Y = 2031;
constexpr int Y_SIZE = 4064;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("noise settings: " + message); }

const json& field(const json& object, const char* name) {
    auto it = object.find(name);
    if (it == object.end()) fail(std::string("No key ") + name);
    return *it;
}

DensityFunctionPtr df(WorldgenRegistries& registries, const json& object, const char* name) {
    return registries.parseDensityFunction(field(object, name));
}

// Climate.Parameter.CODEC: a single float or [min, max].
std::pair<float, float> climateParameter(const json& value) {
    if (value.is_number()) {
        const float v = static_cast<float>(value.get<double>());
        return {v, v};
    }
    if (!value.is_array() || value.size() != 2) fail("Climate parameter must be a number or [min, max]");
    const float min = static_cast<float>(value[0].get<double>());
    const float max = static_cast<float>(value[1].get<double>());
    if (min > max) fail("Climate parameter: min > max");
    return {min, max};
}

} // namespace

NoiseRouter NoiseRouter::fromJson(const json& json, WorldgenRegistries& registries) {
    NoiseRouter router;
    // RecordCodecBuilder decodes the fields in declaration order.
    router.temperature = df(registries, json, "temperature");
    router.vegetation = df(registries, json, "vegetation");
    router.continents = df(registries, json, "continents");
    router.erosion = df(registries, json, "erosion");
    router.depth = df(registries, json, "depth");
    router.ridges = df(registries, json, "ridges");
    router.chunkSurfaceLevel = df(registries, json, "chunk_surface_level");
    router.finalDensity = df(registries, json, "final_density");
    return router;
}

NoiseSettings NoiseSettings::create(int minY, int height) {
    if (minY < MIN_Y || minY > MAX_Y) fail("min_y out of range");
    if (height < 0 || height > Y_SIZE) fail("height out of range");
    if (minY + height > MAX_Y + 1) fail("min_y + height cannot be higher than: " + std::to_string(MAX_Y + 1));
    if (height % 16 != 0) fail("height has to be a multiple of 16");
    if (minY % 16 != 0) fail("min_y has to be a multiple of 16");
    return NoiseSettings{minY, height};
}

NoiseSettings NoiseSettings::clampToHeightAccessor(int levelMinY, int levelMaxY) const {
    const int newMinY = std::max(minY, levelMinY);
    const int newHeight = std::min(minY + height, levelMaxY + 1) - newMinY;
    return NoiseSettings{newMinY, newHeight};
}

TerrainSettings::BlockState* TerrainSettings::parseBlockState(const json& value) {
    std::string name;
    std::map<std::string, std::string> properties;
    if (value.is_string()) {
        // BlockStateParser syntax: minecraft:name[key=value,...].
        const std::string text = value.get<std::string>();
        const size_t open = text.find('[');
        name = text.substr(0, open);
        if (open != std::string::npos) {
            const size_t close = text.rfind(']');
            if (close == std::string::npos || close < open) fail("Malformed block state " + text);
            std::string body = text.substr(open + 1, close - open - 1);
            size_t start = 0;
            while (start < body.size()) {
                size_t comma = body.find(',', start);
                if (comma == std::string::npos) comma = body.size();
                const std::string pair = body.substr(start, comma - start);
                const size_t eq = pair.find('=');
                if (eq == std::string::npos) fail("Malformed block state " + text);
                properties[pair.substr(0, eq)] = pair.substr(eq + 1);
                start = comma + 1;
            }
        }
    } else if (value.is_object()) {
        name = field(value, "Name").get<std::string>();
        auto props = value.find("Properties");
        if (props != value.end()) {
            for (const auto& [key, v] : props->items()) properties[key] = v.get<std::string>();
        }
    } else {
        fail("Not a block state: " + value.dump());
    }
    name = WorldgenRegistries::normalizeKey(name);
    BlockState* state = world::level::block::Blocks::resolveState(name, properties);
    if (state == nullptr) fail("Unknown block " + name);
    return state;
}

std::shared_ptr<const TerrainSettings> TerrainSettings::fromJson(const json& json, WorldgenRegistries& registries) {
    auto settings = std::make_shared<TerrainSettings>();
    const nlohmann::json& noise = field(json, "noise");
    settings->noiseSettings = NoiseSettings::create(field(noise, "min_y").get<int>(), field(noise, "height").get<int>());
    settings->defaultBlock = parseBlockState(field(json, "default_block"));
    settings->defaultFluid = parseBlockState(field(json, "default_fluid"));
    settings->noiseRouter = NoiseRouter::fromJson(field(json, "noise_router"), registries);
    const nlohmann::json& materialRule = field(json, "material_rule");
    if (!materialRule.is_string()) fail("material_rule must name a registry entry");
    settings->materialRule = WorldgenRegistries::normalizeKey(materialRule.get<std::string>());
    for (const nlohmann::json& point : field(json, "spawn_target")) {
        SpawnTargetPoint target;
        for (const auto& [key, span] : point.items()) {
            const auto [min, max] = climateParameter(span);
            target.parameters.push_back({registries.densityFunction(key), min, max});
        }
        settings->spawnTarget.push_back(std::move(target));
    }
    settings->seaLevel = field(json, "sea_level").get<int>();
    settings->disableMobGeneration = field(json, "disable_mob_generation").get<bool>();
    auto aquifers = json.find("aquifers");
    if (aquifers != json.end() && !aquifers->is_null()) {
        Aquifer::Config config;
        config.barrierNoise = df(registries, *aquifers, "barrier");
        config.fluidLevelFloodednessNoise = df(registries, *aquifers, "fluid_level_floodedness");
        config.fluidLevelSpreadNoise = df(registries, *aquifers, "fluid_level_spread");
        config.lavaNoise = df(registries, *aquifers, "lava");
        config.exclusion = df(registries, *aquifers, "exclusion");
        config.surfaceLevel = df(registries, *aquifers, "surface_level");
        settings->aquifers = std::move(config);
    }
    settings->useLegacyRandomSource = field(json, "legacy_random_source").get<bool>();
    auto debug = json.find("debug_functions");
    if (debug != json.end()) {
        for (const nlohmann::json& entry : *debug) {
            settings->debugFunctions.push_back(
                {field(entry, "label").get<std::string>(), df(registries, entry, "function")});
        }
    }
    return settings;
}

std::shared_ptr<const TerrainSettings> TerrainSettings::load(const std::string& key, WorldgenRegistries& registries) {
    try {
        return fromJson(registries.readEntry("noise_settings", key), registries);
    } catch (const std::exception& e) {
        throw std::runtime_error("worldgen/noise_settings " + WorldgenRegistries::normalizeKey(key) + ": " + e.what());
    }
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
