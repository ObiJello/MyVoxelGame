// File: src/server/world/storage/anvil/PlayerDataStore.hpp
//
// playerdata/<uuid>.dat — GZIP NBT, one file per player, exactly where and how
// vanilla keeps it.
//
// THE UUID. Minecraft's offline convention: a type-3 (MD5) UUID over the bytes
// of "OfflinePlayer:<name>". Deriving it the same way means an ObeyCraft
// player's file loads directly in an offline-mode Minecraft under the same
// username — which is the whole point of writing vanilla's format.
//
// It also makes the name load-bearing: a name that changes between joins is a
// different UUID and therefore a different, empty player file. See
// IntegratedServer's name resolution, which now hands a returning solo player
// the same name every session instead of one derived from a connection id.
#pragma once

#include "server/world/storage/anvil/PlayerUuid.hpp"
#include "server/world/storage/anvil/SaveRoot.hpp"

#include "common/core/Uuid.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Server { class ServerPlayer; }
namespace Game { class Mob; }

namespace Game::Anvil {

    // Writes playerdata/<uuid>.dat via temp + rename, keeping the previous
    // generation as <uuid>.dat_old. Returns false only on a real I/O failure.
    // `rootVehicle` (with the UUID of the entity the player sits on) is the
    // vehicle leaving with the player — MC's "RootVehicle"; null when none.
    bool WritePlayerData(const SaveRoot& root, const Server::ServerPlayer& player,
                         int dataVersion, std::string& error,
                         const Game::Mob* rootVehicle = nullptr, const Game::Uuid* attachUuid = nullptr);

    // The player file's root compound, uncompressed — what WritePlayerData
    // writes, and what an `nbt=` selector tests a player against.
    bool BuildPlayerNbt(const Server::ServerPlayer& player, int dataVersion, std::vector<uint8_t>& out,
                        std::string& error, const Game::Mob* rootVehicle = nullptr,
                        const Game::Uuid* attachUuid = nullptr);

    // Restores position, rotation, health, food, XP, game mode and inventory.
    // Returns false with an EMPTY error when the player has simply never been
    // saved — a first join, not a problem.
    bool ReadPlayerData(const SaveRoot& root, Server::ServerPlayer& player,
                        std::string& error);

} // namespace Game::Anvil
