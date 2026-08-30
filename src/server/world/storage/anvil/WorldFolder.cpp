// File: src/server/world/storage/anvil/WorldFolder.cpp
#include "server/world/storage/anvil/WorldFolder.hpp"

#include "common/core/Log.hpp"
#include "common/world/level/DimensionId.hpp"
#include "platform/GameDirectory.hpp"

#include <array>
#include <filesystem>
#include <system_error>

namespace Game::Anvil {

    std::string SanitiseFolderName(const std::string& worldName) {
        // MC LevelStorageSource: the reserved set is / \ : * ? " < > | plus
        // control characters. Vanilla also refuses the Windows device names,
        // but those cannot be produced by our own dedup suffixes.
        std::string out;
        out.reserve(worldName.size());
        for (const char c : worldName) {
            const bool reserved = c == '/' || c == '\\' || c == ':' || c == '*' ||
                                  c == '?' || c == '"'  || c == '<' || c == '>' ||
                                  c == '|' || static_cast<unsigned char>(c) < 0x20;
            out.push_back(reserved ? '_' : c);
        }
        // A name that is empty, all dots, or has trailing dots/spaces is not a
        // usable directory on Windows.
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
        if (out.empty()) out = "World";
        return out;
    }

    std::optional<SaveRoot> RootForWorldName(const std::string& worldName, std::string& reason) {
        const std::string folder = SanitiseFolderName(worldName);
        const std::filesystem::path path =
            std::filesystem::path(Platform::g_gameDirectory.GetSavesDirectory()) / folder;
        return SaveRoot::Open(path.string(), reason);
    }

    bool EnsureDirectories(const SaveRoot& root, std::string& error) {
        std::error_code ec;

        auto make = [&](const std::filesystem::path& p) {
            std::filesystem::create_directories(p, ec);
            if (ec) {
                error = "cannot create " + p.string() + ": " + ec.message();
                return false;
            }
            return true;
        };

        if (!make(root.Root())) return false;
        if (!make(root.PlayerDataDir())) return false;

        for (const DimensionId dim : {DimensionId::Overworld, DimensionId::Nether, DimensionId::End}) {
            if (!make(root.RegionDir(dim)))   return false;
            if (!make(root.EntitiesDir(dim))) return false;
            if (!make(root.PoiDir(dim)))      return false;
            if (!make(root.DataDir(dim)))     return false;
        }
        return true;
    }

    bool CreateWorldFolder(const SaveRoot& root, const LevelDatData& data,
                           int dataVersion, std::string& error) {
        if (!EnsureDirectories(root, error)) return false;
        if (!WriteLevelDat(root, data, dataVersion, error)) return false;
        Log::Info("[Anvil] world folder ready: %s", root.Root().string().c_str());
        return true;
    }

    bool LooksLikeWorld(const SaveRoot& root) {
        std::error_code ec;
        return std::filesystem::is_regular_file(root.LevelDat(), ec);
    }

} // namespace Game::Anvil
