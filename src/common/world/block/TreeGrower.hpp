// File: src/common/world/block/TreeGrower.hpp
//
// Trees growing in the LIVE world — MC's
//   world/level/block/grower/TreeGrower.java      (which tree, 2x2 search,
//                                                  sapling removal / reset)
//   world/level/block/SaplingBlock.java           (stage, light, 1 in 7, 45 %)
//   world/level/block/MangrovePropaguleBlock.java (hanging age, no light gate)
//   world/level/block/AzaleaBlock.java            (bone meal → azalea tree)
//   world/level/block/NetherFungusBlock.java      (bone meal → planted fungus)
//
// The trees themselves are NOT placed here. A grown tree is the very
// configured feature worldgen places (TreeGrower names feature keys, and so
// do we: "minecraft:oak", "minecraft:mega_spruce", …), run by the terrain
// library against the live world through the server's adapter
// (server/level/LiveFeatureLevel). Common code only sees that adapter as the
// LiveFeatures hooks below, which the server installs at startup; a level
// without them (the client's prediction) never grows a tree.
//
// The engine's mod saplings follow their mods' growers: the Twilight Forest
// and the Aether register 1.21.1 TreeGrowers (secondaryChance / secondary
// mega / secondary flowers, flag-4 sapling removal), which are modelled as
// the legacy grower kind; the Hush's whisperwood uses the 26.3 grower.
#pragma once

#include "BlockRegistry.hpp"

#include <array>
#include <cstdint>
#include <string_view>

#include <glm/glm.hpp>

namespace Game {

    class ILevelWrite;
    class JavaRandom;

    namespace LiveFeatures {

        // MC Registries.FEATURE lookup + Feature.place(level, generator,
        // random, origin) against a live level. Installed by the server.
        struct Hooks {
            // `registryAccess().lookupOrThrow(FEATURE).get(key)` is present.
            bool (*hasFeature)(std::string_view featureId) = nullptr;
            // Place the feature. False when it did not place (or when the
            // area around `origin` is not loaded — the live world never
            // loads a chunk to finish a tree).
            bool (*place)(ILevelWrite& level, std::string_view featureId,
                          const glm::ivec3& origin, JavaRandom& random) = nullptr;
            // MC ServerLevel.sendBlockUpdated: re-announce the cell's current
            // state to its watchers.
            void (*sendBlockUpdated)(ILevelWrite& level, const glm::ivec3& pos) = nullptr;
        };

        void SetHooks(const Hooks& hooks);
        const Hooks& GetHooks();

        // Convenience wrappers; all answer false / do nothing without hooks
        // or on a client-side level.
        bool HasFeature(std::string_view featureId);
        bool Place(ILevelWrite& level, std::string_view featureId, const glm::ivec3& origin,
                   JavaRandom& random);
        void SendBlockUpdated(ILevelWrite& level, const glm::ivec3& pos);

    } // namespace LiveFeatures

    // Wires isRandomlyTicking / randomTick / the bone meal trio on every
    // sapling, the mangrove propagule, both azaleas and both nether fungi.
    // Called from BlockRegistry_RegisterGrowth.
    void RegisterTreeGrowth(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
