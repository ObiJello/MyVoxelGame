// File: src/client/renderer/blockentity/SpawnerRenderer.hpp
//
// The mini mob spinning inside a monster spawner's cage. Mirrors MC
// `SpawnerRenderer.java` (extractRenderState + submitEntityInSpawner, with
// TrialSpawnerRenderer.extractSpawnerData's scale):
//
//   translate(0.5, 0.4, 0.5)
//   rotateY(lerp(partialTick, oSpin, spin) * 10)
//   translate(0, -0.2, 0)
//   rotateX(-30)
//   scale(0.53125 / max(1, max(bbWidth, bbHeight)))
//   <the display entity's own renderer, at its origin>
//
// The display entity is the spawner's next SpawnData's type (a baby when the
// compound says so), drawn by MobRenderer's pose path — the one /morph uses —
// so every mob model the game has works in a cage. The chain above folds to
// the pose path's transform exactly: the Y translations commute with the Y
// spin, and rotateX(-30) followed by the entity's own rotateY(180 - 0)
// equals rotateY(180) then rotateX(+30), so the pose is placed at
// (0.5, 0.2, 0.5) in the block with body yaw -spin and a +30 degree tilt
// (MorphPose::tiltDeg) about the feet.
//
// Deviation: MC's display entity is a full client entity loaded from the
// whole SpawnData compound; the pose path draws the type's default look (plus
// baby), so a variant carried in the compound (a sheep's colour, a
// villager's profession) shows as the type's default.
#pragma once

#include "BlockEntityRenderer.hpp"

namespace Render {

    class MobRenderer;

    class SpawnerRenderer : public BlockEntityRenderer {
    public:
        // The frame's mob renderer. Owned by PlatformMain's join scope, which
        // outlives the block-entity pass; cleared before it shuts down.
        static void SetMobRenderer(MobRenderer* renderer);

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;
    };

} // namespace Render
