// File: src/server/world/storage/anvil/WorldSidecar.cpp
#include "server/world/storage/anvil/WorldSidecar.hpp"

#include "common/core/Log.hpp"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Game::Anvil {

    namespace {
        std::filesystem::path SidecarPath(const std::string& worldRoot) {
            return std::filesystem::path(worldRoot) / "data" / "obeycraft.json";
        }
    }

    WorldSidecar ReadWorldSidecar(const std::string& worldRoot) {
        WorldSidecar out;
        const auto path = SidecarPath(worldRoot);

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return out;

        try {
            std::ifstream f(path);
            nlohmann::json j;
            f >> j;
            out.skybox         = j.value("skybox",         out.skybox);
            out.skyboxMode     = j.value("skyboxMode",     out.skyboxMode);
            out.babyModels     = j.value("babyModels",     out.babyModels);
            out.tickFrozen     = j.value("tickFrozen",     out.tickFrozen);
            out.tickRate       = j.value("tickRate",       out.tickRate);
            out.worldType      = j.value("worldType",      out.worldType);
            out.flatPreset     = j.value("flatPreset",     out.flatPreset);
            out.flatLayers     = j.value("flatLayers",     out.flatLayers);
            out.singleBiome    = j.value("singleBiome",    out.singleBiome);
            out.worldgenTweaks = j.value("worldgenTweaks", out.worldgenTweaks);
            out.bonusChest     = j.value("bonusChest",     out.bonusChest);
            if (const auto rp = j.find("resourcePacks"); rp != j.end() && rp->is_array()) {
                out.hasResourcePacks = true;
                for (const auto& e : *rp) if (e.is_string()) out.resourcePacks.push_back(e.get<std::string>());
                if (const auto ip = j.find("incompatibleResourcePacks"); ip != j.end() && ip->is_array()) {
                    for (const auto& e : *ip) if (e.is_string()) out.incompatibleResourcePacks.push_back(e.get<std::string>());
                }
            }
        } catch (const std::exception& e) {
            // Defaults are a working world, so a corrupt sidecar is a warning
            // rather than a reason to hide the world from the list.
            Log::Warning("[Anvil] could not read %s: %s", path.string().c_str(), e.what());
        }
        return out;
    }

    bool WriteWorldSidecar(const std::string& worldRoot, const WorldSidecar& sidecar) {
        const auto path = SidecarPath(worldRoot);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        try {
            nlohmann::json j;
            j["skybox"]         = sidecar.skybox;
            j["skyboxMode"]     = sidecar.skyboxMode;
            j["babyModels"]     = sidecar.babyModels;
            j["tickFrozen"]     = sidecar.tickFrozen;
            j["tickRate"]       = sidecar.tickRate;
            j["worldType"]      = sidecar.worldType;
            j["flatPreset"]     = sidecar.flatPreset;
            j["flatLayers"]     = sidecar.flatLayers;
            j["singleBiome"]    = sidecar.singleBiome;
            j["worldgenTweaks"] = sidecar.worldgenTweaks;
            j["bonusChest"]     = sidecar.bonusChest;
            if (sidecar.hasResourcePacks) {
                j["resourcePacks"]             = sidecar.resourcePacks;
                j["incompatibleResourcePacks"] = sidecar.incompatibleResourcePacks;
            }

            std::ofstream f(path);
            if (!f) return false;
            f << j.dump(2) << '\n';
            return true;
        } catch (const std::exception& e) {
            Log::Warning("[Anvil] could not write %s: %s", path.string().c_str(), e.what());
            return false;
        }
    }

} // namespace Game::Anvil
