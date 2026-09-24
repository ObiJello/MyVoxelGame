// File: src/common/world/block/AercloudBlock.hpp
//
// The Aether's aerclouds (AercloudBlock, BlueAercloudBlock — cold and golden
// are plain AercloudBlocks): the one collision shape in the engine that
// depends on WHO is asking and on the block above, plus the constants of
// their entityInside hooks as the local player's physics applies them.
//
// Header-only: the physics (Physics.cpp, for the player and every mob), the
// registry's entity-less shape (BlockRegistry.cpp) and the server hooks
// (BlockBehaviors.cpp) all read it.
#pragma once

#include "common/world/block/Blocks.hpp"

#include <glm/glm.hpp>

namespace Game::Aercloud {

    inline bool IsAercloud(BlockID id) {
        return id == BlockID::ColdAercloud || id == BlockID::BlueAercloud ||
               id == BlockID::GoldenAercloud;
    }

    // AercloudBlock.COLLISION_SHAPE = Block.box(0, 0, 0, 16, 0.01, 16): a floor
    // 0.01 PIXELS thick (Block.box is in sixteenths), so a thing resting on
    // it stands 0.000625 above the cell bottom — inside the cloud.
    inline constexpr float kFloorHeight = 0.01f / 16.0f;
    // AercloudBlock.FALLING_COLLISION_SHAPE = Shapes.box(0, 0, 0, 1, 0.9, 1)
    // (Shapes.box is in blocks): what something falling faster lands on.
    inline constexpr float kFallingHeight = 0.9f;
    // getCollisionShape's "falling fast enough" test: entity.fallDistance > 2.5.
    inline constexpr float kFallingDistance = 2.5f;

    // MC CollisionContext, reduced to what AercloudBlock.getCollisionShape
    // reads. `entity` false is CollisionContext.empty() — a query with no
    // entity behind it (spawn-space tests, fluids, placement, particles).
    struct Asker {
        bool  entity       = false;
        float fallDistance = 0.0f;
        bool  fallFlying   = false;   // LivingEntity.isFallFlying (elytra)
    };

    // AercloudBlock.getCollisionShape, with BlueAercloudBlock's
    // getDefaultCollisionShape override. `above` is the block at pos.above().
    // Returns false for an empty shape; otherwise writes the one cell-local
    // box.
    //
    //   default shape: the thin floor — except a blue cloud asked BY AN
    //                  ENTITY, whose default is empty (entities pass into it
    //                  and are launched; an entity-less query still sees the
    //                  floor, `context == CollisionContext.empty()`).
    //   1. default non-empty AND an aercloud above → a full block, so the
    //      lower layers of a cloud hold you and you sink into the top layer
    //      only (the fall-damage quirk the Java comment describes).
    //   2. an entity falling farther than 2.5 blocks (not gliding) → the
    //      0.9-tall falling shape: it lands INSIDE the cloud, where
    //      entityInside resets the fall and slows the sink.
    //   3. otherwise the default.
    inline bool CollisionBox(BlockID id, BlockID above, const Asker& asker,
                             glm::vec3& lo, glm::vec3& hi) {
        const bool defaultEmpty = (id == BlockID::BlueAercloud) && asker.entity;
        lo = glm::vec3(0.0f);
        if (!defaultEmpty && IsAercloud(above)) {
            hi = glm::vec3(1.0f);
            return true;
        }
        if (asker.entity && asker.fallDistance > kFallingDistance && !asker.fallFlying) {
            hi = glm::vec3(1.0f, kFallingHeight, 1.0f);
            return true;
        }
        if (defaultEmpty) return false;
        hi = glm::vec3(1.0f, kFloorHeight, 1.0f);
        return true;
    }

    // ── entityInside, as the local player's physics applies it ─────────────
    //
    // The player's physics runs in blocks per SECOND at the frame rate
    // (Physics.cpp), MC's in blocks per tick at 20 Hz; these are MC's
    // per-tick results carried over.

    // AercloudBlock.entityInside multiplies a downward deltaMovement.y by
    // 0.005 after the move, and travel then subtracts gravity and applies the
    // 0.98 drag: v' = (0.005·v − 0.08)·0.98, whose fixed point is
    // −0.0787861 blocks a tick. That is the sink rate inside a cloud —
    // −1.5757 blocks a second — whatever speed the thing arrived with.
    inline constexpr float kSinkSpeedPerSecond = -1.5757210f;

    // BlueAercloudBlock.entityInside sets deltaMovement.y to 2.0; travel then
    // runs the air physics ((v − 0.08)·0.98 a tick), which carries a player
    // 18.02 blocks up. The player's physics here has no air drag (its jump is
    // tuned the same way), so the launch is the speed that reaches the same
    // apex under its gravity: sqrt(2 · 32.656 · 18.02) = 34.307 blocks/s.
    inline constexpr float kBlueLaunchSpeedPerSecond = 34.306566f;

} // namespace Game::Aercloud
