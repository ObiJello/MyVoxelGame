// File: src/server/world/storage/anvil/SaveRoot.hpp
//
// A capability token proving a directory is one ObeyCraft is allowed to write.
//
// The requirement is "never write a byte into the player's real Minecraft
// world". A runtime `if (readOnly) return;` cannot deliver that on its own,
// because the save path is not one funnel: ChunkCache saves through its own
// saver reference on eviction and in its destructor, and level.dat,
// session.lock and playerdata sit above ChunkProvider entirely.
//
// So the guarantee is moved into the type system. SaveRoot has a private
// constructor and exactly one factory, and EVERY write API in the Anvil stack
// takes `const SaveRoot&` — none takes a bare path. A directory under the
// real .minecraft install cannot produce one, which makes "write into a real
// Minecraft world" not merely forbidden but unrepresentable.
#pragma once

#include "common/world/level/DimensionId.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace Game::Anvil {

    class SaveRoot {
    public:
        // The only way to make one. Returns nullopt — and sets `reason` — when
        // `worldPath` is empty, is not under the obeycraft saves directory, or
        // resolves under the player's Minecraft installation. Symlinks are
        // resolved first, so a link planted inside obeycraft/saves that points
        // at .minecraft/saves is rejected too.
        static std::optional<SaveRoot> Open(const std::string& worldPath, std::string& reason);

        const std::filesystem::path& Root() const { return m_root; }

        // Overworld lives at the world root; the nether and end sit under
        // DIM-1/ and DIM1/ (DimensionSaveSubdir).
        std::filesystem::path Dimension(DimensionId dim) const;

        std::filesystem::path RegionDir  (DimensionId dim) const { return Dimension(dim) / "region"; }
        std::filesystem::path EntitiesDir(DimensionId dim) const { return Dimension(dim) / "entities"; }
        std::filesystem::path PoiDir     (DimensionId dim) const { return Dimension(dim) / "poi"; }
        std::filesystem::path DataDir    (DimensionId dim) const { return Dimension(dim) / "data"; }

        std::filesystem::path LevelDat()    const { return m_root / "level.dat"; }
        std::filesystem::path LevelDatOld() const { return m_root / "level.dat_old"; }
        std::filesystem::path SessionLock() const { return m_root / "session.lock"; }
        std::filesystem::path PlayerDataDir() const { return m_root / "playerdata"; }

    private:
        explicit SaveRoot(std::filesystem::path root) : m_root(std::move(root)) {}

        std::filesystem::path m_root;
    };

} // namespace Game::Anvil
