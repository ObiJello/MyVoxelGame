// File: src/server/world/storage/anvil/WorldSidecar.hpp
//
// <world>/data/obeycraft.json — the settings vanilla has nowhere to put.
//
// level.dat stays byte-vanilla on purpose: it is the file Minecraft reads, and
// stuffing engine-only keys into it would work (MC ignores unknown tags) but
// would also make every third-party tool show junk. These six live beside it
// instead, in the world folder, so they travel with a copied world.
//
// Everything with a vanilla home — name, seed, game mode, difficulty, time,
// gamerules — is NOT here. level.dat is authoritative for those.
#pragma once

#include <string>
#include <vector>

namespace Game::Anvil {

    struct WorldSidecar {
        std::string skybox      = "vanilla";
        int         skyboxMode  = 2;      // 0 static, 1 darken, 2 darken+celestials
        // Options → World Settings → Baby Models: "new" (MC 26.1's dedicated
        // baby meshes + the rabbit remodel) or "classic" (the pre-26.1
        // shrunken-adult babies). Visual only — see Render::BabyModelLook.
        std::string babyModels  = "new";
        // /tick freeze and /tick rate, kept across sessions. Vanilla's
        // ServerTickRateManager is rebuilt fresh every launch (nothing in
        // level.dat), so a frozen world thaws on reopen; this game keeps the
        // state the player left. Written by IntegratedServer::PersistTickState
        // on every /tick freeze|unfreeze|rate, read at world open.
        bool        tickFrozen  = false;
        float       tickRate    = 20.0f;
        int         worldType   = 0;      // 0 Default, 1 Superflat, 2 Large Biomes, 3 Amplified, 4 Single Biome
        std::string flatPreset;
        std::string flatLayers;
        std::string singleBiome = "minecraft:plains";
        std::string worldgenTweaks;
        bool        bonusChest  = false;
        // Resource packs chosen while IN this world (Options → Resource
        // Packs): options.txt's resourcePacks / incompatibleResourcePacks,
        // per world. Absent (hasResourcePacks false) = the global selection.
        bool                     hasResourcePacks = false;
        std::vector<std::string> resourcePacks;
        std::vector<std::string> incompatibleResourcePacks;
    };

    // Missing or unreadable returns defaults — a world without a sidecar is a
    // perfectly good world, which is exactly the case for one dragged in from
    // a Minecraft install.
    WorldSidecar ReadWorldSidecar(const std::string& worldRoot);

    bool WriteWorldSidecar(const std::string& worldRoot, const WorldSidecar& sidecar);

} // namespace Game::Anvil
