// File: src/server/level/LiveFeatureLevel.hpp
//
// The terrain library's configured features, run against the LIVE world.
//
// The library already carries MC's feature code — TreeFeature with every
// trunk / foliage / root placer and tree decorator, the huge fungus, the mod
// trees — but only ever ran it inside a generating region. This is the other
// half of MC's split: the same Feature.place against a ServerLevel, which is
// how a sapling grows (TreeGrower.growTree), a fungus is bone-mealed
// (NetherFungusBlock) and anything else places a feature outside worldgen.
// Worldgen trees and grown trees are therefore one piece of code.
//
// The adapter (LiveFeatureLevel.cpp) is a WorldGenLevel over Game::World:
//   * block states cross the library/game boundary through the same name +
//     property resolution chunk hand-off uses (BlockStateRegistry) one way,
//     and the library's own Blocks::resolveState the other, both cached;
//   * writes go through World::SetBlock with the feature's own MC flags
//     (19 for a tree, 3 for a fungus, …) — neighbour updates, light, the
//     client broadcast and block entities all happen as for any other edit;
//   * TreeFeature's closing StructureTemplate.updateShapeAtEdge pass runs
//     the game's real BlockState.updateShape (World::UpdateShape), not the
//     library's worldgen emulation of it;
//   * only LOADED chunks are ever read or written. A placement is refused
//     up front unless every chunk within two of the origin is resident, so
//     a tree is never half-built across a chunk the live world would have to
//     load or generate synchronously.
#pragma once

#include <cstdint>
#include <string_view>

#include <glm/glm.hpp>

namespace Game {

    class JavaRandom;
    class World;

    // Place the configured feature registered as `featureId` — the MC key
    // ("minecraft:oak", "minecraft:crimson_fungus_planted"), a Twilight
    // Forest id ("twilightforest:tree/canopy_tree"), "aether:skyroot_tree",
    // "hush:whisperwood" — at `origin`. `random` is the level random the
    // caller would hand MC's Feature.place. False when the id is unknown,
    // the area is not loaded, or the feature declined to place.
    bool PlaceLiveFeature(World& world, std::string_view featureId, const glm::ivec3& origin,
                          JavaRandom& random);

    // Whether `featureId` names a feature PlaceLiveFeature can run.
    bool HasLiveFeature(std::string_view featureId);

    // Installs the LiveFeatures hooks (common/world/block/TreeGrower.hpp)
    // that let common block code grow trees. Called once at server start.
    void InstallLiveFeaturePlacement();

} // namespace Game
