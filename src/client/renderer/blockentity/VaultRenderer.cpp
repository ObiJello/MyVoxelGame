// File: src/client/renderer/blockentity/VaultRenderer.cpp
#include "VaultRenderer.hpp"

#include "client/renderer/entity/ItemEntityRenderer.hpp"
#include "client/renderer/environment/EntityEnvironment.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/block/entity/VaultBlockEntity.hpp"

#include <cmath>

namespace Render {

    namespace {
        ItemEntityRenderer* g_itemRenderer = nullptr;

        // MC Mth.wrapDegrees(float).
        float WrapDegrees(float angle) {
            float wrapped = std::fmod(angle, 360.0f);
            if (wrapped >= 180.0f) wrapped -= 360.0f;
            if (wrapped < -180.0f) wrapped += 360.0f;
            return wrapped;
        }
    } // namespace

    void VaultRenderer::SetItemRenderer(ItemEntityRenderer* renderer) { g_itemRenderer = renderer; }

    void VaultRenderer::Render(const Game::BlockEntity& be, float partialTick,
                               const glm::mat4& proj, const glm::mat4& view,
                               const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("BE.Vault");
        if (!g_itemRenderer) return;
        const auto* vault = dynamic_cast<const Game::VaultBlockEntity*>(&be);
        if (!vault || !vault->ShouldDisplayActiveEffects()) return;
        const Game::ItemStack& display = vault->GetSharedData().displayItem;
        if (display.IsEmpty()) return;

        // Mth.rotLerp(partialTick, previousSpin, currentSpin).
        const float spin = vault->GetPreviousSpin() +
                           partialTick * WrapDegrees(vault->GetCurrentSpin() - vault->GetPreviousSpin());
        const glm::ivec3 pos = be.GetWorldPos();
        const glm::vec3 light =
            EntityEnvironment::LightColor(EntityEnvironment::LevelLightCoordsAt(pos));
        g_itemRenderer->RenderCluster(display, glm::dvec3(pos.x + 0.5, pos.y + 0.4, pos.z + 0.5), spin, 1.0f,
                                      light, proj, view, cameraPos);
    }

} // namespace Render
