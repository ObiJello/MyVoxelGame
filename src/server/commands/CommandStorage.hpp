// File: src/server/commands/CommandStorage.hpp
//
// MC net.minecraft.world.level.storage.CommandStorage — the `/data storage`
// compounds, keyed by a resource id ("minecraft:foo", "mypack:bar"). Saved
// the way vanilla saves them, one file per namespace:
//   <world>/data/command_storage_<namespace>.dat
//   { data: { contents: { <path>: {...}, ... } }, DataVersion }
// so a world's storage survives a trip to vanilla and back. Loaded per
// namespace on first use; written on the world's saves.
//
// Server thread only.
#pragma once

#include "server/world/storage/NBTParser.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Server::CommandStorage {

    // "foo" -> "minecraft:foo"; false for an id that is not [ns:]path of
    // MC Identifier's characters.
    bool NormalizeId(const std::string& text, std::string& out);

    // A copy of the stored compound (an empty one when nothing is stored).
    std::shared_ptr<::World::NBTTagCompound> Get(const std::string& id);
    // Store a copy; an empty compound removes the entry (MC Container.put).
    void Set(const std::string& id, const ::World::NBTTagCompound& data);
    // Every id holding data, sorted (completion).
    std::vector<std::string> Ids();

    // Write the namespaces that changed (the world save).
    void Save();
    // After the final save: forget everything (the next world reads its own).
    void Close();

} // namespace Server::CommandStorage
