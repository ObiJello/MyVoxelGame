// File: src/client/renderer/blockentity/SpawnerRenderer.cpp
#include "SpawnerRenderer.hpp"

#include "client/renderer/core/Frustum.hpp"
#include "client/renderer/entity/MobRenderer.hpp"
#include "common/entity/Morph.hpp"
#include "common/world/block/entity/SpawnerBlockEntity.hpp"

#include <algorithm>
#include <vector>

namespace Render {

    namespace {
        MobRenderer* g_mobRenderer = nullptr;

        // Every plane passes everything: the dispatcher has already culled
        // this block entity by its section, and the pose path's own frustum
        // test is written for the world-space entity frustum, which the
        // block-entity pass does not carry.
        Frustum PassAllFrustum() {
            Frustum f;
            for (glm::vec4& plane : f.planes) plane = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            return f;
        }
    } // namespace

    void SpawnerRenderer::SetMobRenderer(MobRenderer* renderer) { g_mobRenderer = renderer; }

    void SpawnerRenderer::Render(const Game::BlockEntity& be, float partialTick,
                                 const glm::mat4& proj, const glm::mat4& view,
                                 const glm::vec3& cameraPos) {
        if (!g_mobRenderer) return;
        const auto* spawner = dynamic_cast<const Game::SpawnerBlockEntity*>(&be);
        if (!spawner || !spawner->HasNextSpawnData()) return;
        const Game::SpawnData& data = spawner->GetNextSpawnData();
        if (!data.hasType) return;   // getOrCreateDisplayEntity: no id, no entity

        const uint32_t code = Game::Morph::WithBaby(
            Game::Morph::Encode(Game::Morph::Kind::Mob, static_cast<uint32_t>(data.type)), data.baby);
        if (!Game::Morph::IsValid(code)) return;

        // TrialSpawnerRenderer.extractSpawnerData: spin in degrees, the
        // cage scale shrunk for anything longer than a block.
        const float spin = static_cast<float>(spawner->GetOSpin() +
                               (spawner->GetSpin() - spawner->GetOSpin()) * partialTick) * 10.0f;
        const Game::Morph::Dims dims = Game::Morph::DimsOf(code);
        float scale = 0.53125f;
        const float maxLength = std::max(dims.width, dims.height);
        if (maxLength > 1.0f) scale /= maxLength;

        const glm::ivec3 pos = be.GetWorldPos();
        MobRenderer::MorphPose pose;
        pose.code     = code;
        pose.position = glm::dvec3(pos.x + 0.5, pos.y + 0.2, pos.z + 0.5);
        pose.bodyYaw  = -spin;
        pose.headYaw  = -spin;
        pose.pitch    = 0.0f;
        pose.scale    = scale;
        pose.tiltDeg  = 30.0f;
        // The display entity is never ticked: its age stays 0.
        pose.ageTicks = 0.0f;
        pose.seed     = static_cast<uint32_t>(pos.x * 73856093) ^ static_cast<uint32_t>(pos.y * 19349663) ^
                        static_cast<uint32_t>(pos.z * 83492791);

        static const Frustum kPassAll = PassAllFrustum();
        const std::vector<MobRenderer::MorphPose> poses{ pose };
        g_mobRenderer->RenderMorphs(proj, view, cameraPos, kPassAll, poses, partialTick);
    }

} // namespace Render
