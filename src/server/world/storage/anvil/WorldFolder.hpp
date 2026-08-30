// File: src/server/world/storage/anvil/WorldFolder.hpp
//
// Creating and recognising an ObeyCraft world on disk.
//
// The layout is exactly vanilla's, because the whole point is that the folder
// can be copied into .minecraft/saves and played:
//
//   <world>/
//     level.dat  level.dat_old  session.lock  icon.png
//     region/  entities/  poi/  data/  playerdata/
//     DIM-1/{region,entities,poi}/          nether
//     DIM1/ {region,entities,poi}/          end
//
// poi/ is created and left empty for now — vanilla tolerates that, and the
// consequence (nether portals in a copied world are not linked for Minecraft,
// which will excavate new ones) is a stated limitation rather than a surprise.
#pragma once

#include "server/world/storage/anvil/LevelDat.hpp"
#include "server/world/storage/anvil/SaveRoot.hpp"

#include <optional>
#include <string>

namespace Game::Anvil {

    // Create the directory tree and an initial level.dat. Safe to call on a
    // folder that already exists — directories are created only if missing —
    // but it DOES rewrite level.dat, so callers that only want the tree should
    // use EnsureDirectories.
    bool CreateWorldFolder(const SaveRoot& root, const LevelDatData& data,
                           int dataVersion, std::string& error);

    // Just the directories.
    bool EnsureDirectories(const SaveRoot& root, std::string& error);

    // Resolve <savesDir>/<name> into a SaveRoot, creating nothing. Returns
    // nullopt when the name escapes the saves directory.
    std::optional<SaveRoot> RootForWorldName(const std::string& worldName, std::string& reason);

    // A folder Minecraft would list: it has a level.dat.
    bool LooksLikeWorld(const SaveRoot& root);

    // Turn a world NAME into a directory name. Vanilla sanitises the same way
    // (LevelStorageSource.getLevelPath / FileUtil.findAvailableName): reserved
    // characters become '_', because a world called "Bob's / World" must not
    // become a path traversal or an unwritable name on Windows.
    std::string SanitiseFolderName(const std::string& worldName);

} // namespace Game::Anvil
