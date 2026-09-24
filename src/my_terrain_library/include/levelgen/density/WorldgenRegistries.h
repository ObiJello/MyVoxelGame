#pragma once

#include "levelgen/density/DensityFunction.h"

#include "external/json.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// The worldgen registries the 26.3 density function codec reads from
// (Registries.NOISE and Registries.DENSITY_FUNCTION), backed by the datapack
// JSON under <data root>/<namespace>/worldgen/{noise,density_function}/. A
// registry entry is read and decoded the first time something names it, then
// kept. Entries can also be registered from code - the engine's own
// dimensions build some of theirs in C++ - and a registered entry wins over a
// file of the same name.
//
// The codec mirrors DensityFunction.CODEC: a number is a constant, a string is
// a registry reference (DensityFunctions.HolderHolder), an object dispatches on
// "type" to the function's MapCodec. Construction goes straight to each
// class's constructor, as the codecs do, never through the DensityFunctions
// factories (which fold constants). Decoding errors throw std::runtime_error
// naming the entry and the codec's complaint.

namespace minecraft {
namespace levelgen {
namespace density {

class CubicSpline;

// The noise registry the density functions name ("minecraft:ridge").
using NoiseRegistryMap = std::unordered_map<std::string, std::shared_ptr<const synth::NormalNoise>>;

class WorldgenRegistries {
public:
    // The process-wide registries over the data root (MC_DATA_ROOT, else the
    // first "data" directory walking up from the working directory).
    static WorldgenRegistries& get();

    explicit WorldgenRegistries(std::filesystem::path dataRoot);

    WorldgenRegistries(const WorldgenRegistries&) = delete;
    WorldgenRegistries& operator=(const WorldgenRegistries&) = delete;

    const std::filesystem::path& dataRoot() const { return m_dataRoot; }

    // Holder<NormalNoise> for a registry key; throws when there is none.
    NoiseHolder noise(const std::string& key);
    // Every noise decoded or registered so far, keyed by full identifier.
    // RandomState resolves the keys a density function names through
    // noiseDefinition, so an entry decoded later is still found.
    std::shared_ptr<const synth::NormalNoise> noiseDefinition(const std::string& key);

    // The registry function itself (what a reference resolves to).
    DensityFunctionPtr densityFunctionValue(const std::string& key);
    // A reference to it: DensityFunctions.HolderHolder(Holder.Reference).
    DensityFunctionPtr densityFunction(const std::string& key);

    void registerNoise(const std::string& key, std::shared_ptr<const synth::NormalNoise> noise);
    void registerDensityFunction(const std::string& key, DensityFunctionPtr function);

    // DensityFunction.CODEC and friends, for the noise-settings decoder.
    DensityFunctionPtr parseDensityFunction(const nlohmann::json& json);
    NoiseHolder parseNoiseHolder(const nlohmann::json& json);
    std::shared_ptr<const CubicSpline> parseSpline(const nlohmann::json& json);

    // Reads <data root>/<namespace>/worldgen/<registry>/<path>.json for key
    // "namespace:path"; throws when there is no such file.
    nlohmann::json readEntry(const std::string& registry, const std::string& key) const;
    bool hasEntry(const std::string& registry, const std::string& key) const;

    // Identifier.parse's default namespace ("ridge" -> "minecraft:ridge").
    static std::string normalizeKey(const std::string& key);

private:
    std::filesystem::path entryPath(const std::string& registry, const std::string& key) const;

    DensityFunctionPtr decodeTyped(const nlohmann::json& json);

    std::filesystem::path m_dataRoot;

    // Recursive: decoding one entry decodes the entries it references.
    std::recursive_mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<const synth::NormalNoise>> m_noises;
    std::unordered_map<std::string, DensityFunctionPtr> m_functions;
    std::unordered_map<std::string, bool> m_decoding;   // cycle guard
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
