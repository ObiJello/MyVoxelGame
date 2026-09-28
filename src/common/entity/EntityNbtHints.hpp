// File: src/common/entity/EntityNbtHints.hpp
//
// What `/summon <entity> <pos> {…}` can usefully say about an entity type:
// the compound keys the server's entity loader (server/world/storage/anvil/
// EntityNbt.cpp ApplyMobNbt, the mod mobs' LoadModNbt) actually applies to
// that type, each with a few ready-to-insert SNBT values. The chat's
// completion for Cmd::Arg::EntityNbt reads it.
//
// Keep it in step with the loader: a key belongs here only when reading it
// changes the entity. Values are SNBT as typed — strings quoted, bytes with
// their `b` — and vanilla ids WITHOUT "minecraft:" (the loader accepts both;
// other namespaces stay, they disambiguate).
#pragma once

#include "common/entity/EntityType.hpp"

#include <string>
#include <vector>

namespace Game::EntityNbtHints {

    struct Key {
        std::string              name;     // "variant", "CollarColor"
        std::vector<std::string> values;   // "\"ashen\"", "5b", "{type:\"plains\",profession:\"farmer\",level:1}"
    };

    // The generic keys (CustomName, NoAI, Silent, …) that apply to `type`
    // plus its own, sorted by name.
    std::vector<Key> KeysFor(EntityTypeId type);

} // namespace Game::EntityNbtHints
