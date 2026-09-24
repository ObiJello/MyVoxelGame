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

    namespace {

        // Builds before 2026-09-09 appended the dimension sub-folder twice, so
        // a Nether written by them lives at DIM-1/DIM-1/{region,entities,...}
        // rather than DIM-1/{region,entities,...}. Minecraft's file fixer
        // moves DIM-1/region into the new layout and then fails to delete
        // DIM-1 because the stray folder is still inside it, and this engine
        // now reads DIM-1/region — so without this the player's Nether and End
        // would silently regenerate. Files are moved individually so a world
        // that was already partly played in the fixed layout keeps both sets;
        // a region file present in both places keeps the outer (newer) one.
        void LiftNestedDimensionFolder(const SaveRoot& root, DimensionId dim) {
            const std::string_view sub = DimensionSaveSubdir(dim);
            if (sub.empty()) return;

            const std::filesystem::path outer  = root.Dimension(dim);
            const std::filesystem::path nested = outer / std::filesystem::path(sub);

            std::error_code ec;
            if (!std::filesystem::is_directory(nested, ec)) return;

            size_t moved = 0, kept = 0;
            for (const auto& kindDir : std::filesystem::directory_iterator(nested, ec)) {
                if (!kindDir.is_directory(ec)) continue;
                const std::filesystem::path target = outer / kindDir.path().filename();
                std::filesystem::create_directories(target, ec);
                for (const auto& file : std::filesystem::directory_iterator(kindDir.path(), ec)) {
                    const std::filesystem::path dest = target / file.path().filename();
                    if (std::filesystem::exists(dest, ec)) { ++kept; continue; }
                    std::filesystem::rename(file.path(), dest, ec);
                    if (ec) {
                        Log::Warning("[Anvil] could not lift %s out of %s: %s",
                                     file.path().filename().string().c_str(),
                                     nested.string().c_str(), ec.message().c_str());
                        ec.clear();
                        ++kept;
                    } else {
                        ++moved;
                    }
                }
                // Only an emptied kind folder goes away; anything we could not
                // move stays where a person can still find it.
                std::filesystem::remove(kindDir.path(), ec);
                ec.clear();
            }
            std::filesystem::remove(nested, ec);   // succeeds only when empty
            ec.clear();

            if (moved || kept) {
                Log::Info("[Anvil] lifted %zu file(s) from %s to %s (%zu left behind)",
                          moved, nested.string().c_str(), outer.string().c_str(), kept);
            }
        }

    } // namespace

    bool EnsureDirectories(const SaveRoot& root, std::string& error) {
        std::error_code ec;

        for (const DimensionId dim : Game::kAllDimensions) {
            LiftNestedDimensionFolder(root, dim);   // no-op for the overworld
        }

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

        for (const DimensionId dim : Game::kAllDimensions) {
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
