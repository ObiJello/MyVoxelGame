// File: src/client/renderer/environment/EntityEnvironment.cpp
#include "EntityEnvironment.hpp"
#include "EnvironmentState.hpp"
#include "Lightmap.hpp"
#include "MobEffectEnvironment.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/world/lighting/LightCoords.hpp"
#include "common/world/lighting/ChunkLight.hpp"
#include "common/world/lighting/BlockLightProperties.hpp"
#include "client/renderer/backend/RenderBackend.hpp"
#include "client/renderer/core/RenderOrigin.hpp"
#ifdef HAS_VULKAN
#include "client/renderer/backend/vulkan/VKBackend.hpp"
#endif

#include <algorithm>
#include <cmath>

namespace Render::EntityEnvironment {

    ShaderHandle CreateShader(const std::string& vertexPath, const std::string& fragmentPath) {
        if (!g_renderBackend) return INVALID_SHADER;
        if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
            auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
            const ShaderHandle shader = vk->CreateShaderFromFilesPortal(vertexPath, fragmentPath);
            if (shader != INVALID_SHADER) vk->SetShaderIgnoresCommonMatrices(shader);
            return shader;
#else
            return INVALID_SHADER;
#endif
        }
        return g_renderBackend->CreateShaderFromFiles(vertexPath, fragmentPath);
    }

    float Lit() {
        return EnvironmentState::Get().Frame().skyBrightness;
    }

    float FullBlockLight() {
        // MC lightmap.fsh at block light 15: the block term saturates, then
        // `color - DarknessScale` applies to every texel. Never below the
        // sky-lit value (the frame's skyBrightness has the same pulse taken
        // off an already smaller number).
        const float full = std::max(0.0f, 1.0f - GetMobEffectView().darknessLightmap);
        return std::max(full, Lit());
    }

    int PackedLightAt(int x, int y, int z) {
        namespace L = Game::Lighting;
        const Client::ClientBlockAccess* blocks = Client::g_clientBlockAccess;
        if (!blocks) return L::LightCoords::Pack(0, 15);
        return L::LightCoords::Pack(blocks->GetBrightness(L::LightLayer::Block, x, y, z),
                                    blocks->GetBrightness(L::LightLayer::Sky, x, y, z));
    }

    int LevelLightCoordsAt(const glm::ivec3& pos) {
        namespace L = Game::Lighting;
        const Client::ClientBlockAccess* blocks = Client::g_clientBlockAccess;
        if (!blocks) return L::LightCoords::kFullSky;
        const Game::BlockState state = blocks->GetBlockState(pos.x, pos.y, pos.z);
        if (L::BlockLightProperties::EmissiveRendering(state)) return L::LightCoords::kFullBright;
        const int sky = blocks->GetBrightness(L::LightLayer::Sky, pos.x, pos.y, pos.z);
        const int block = std::max(blocks->GetBrightness(L::LightLayer::Block, pos.x, pos.y, pos.z),
                                   L::BlockLightProperties::Emission(state));
        return L::LightCoords::Pack(block, sky);
    }

    int PackedLightAt(const glm::dvec3& world) {
        // BlockPos.containing: floor of each component.
        return PackedLightAt(static_cast<int>(std::floor(world.x)),
                             static_cast<int>(std::floor(world.y)),
                             static_cast<int>(std::floor(world.z)));
    }

    glm::vec3 LightColor(int packedLight) {
        namespace L = Game::Lighting;
        const int block = L::LightCoords::Block(packedLight);
        const int sky = L::LightCoords::Sky(packedLight);
        if (!Lightmap::Enabled()) {
            return glm::vec3(block >= 15 ? FullBlockLight() : Lit());
        }
        return Lightmap::Get().SampleFor(EnvironmentState::Get().Frame(), block, sky);
    }

    glm::vec3 LitAt(const glm::dvec3& probe) {
        return LightColor(PackedLightAt(probe));
    }

    glm::vec3 FullBlockAt(const glm::dvec3& probe) {
        namespace L = Game::Lighting;
        return LightColor(L::LightCoords::WithBlock(PackedLightAt(probe), 15));
    }

    uint32_t ToRGBA8(const glm::vec3& light) {
        auto byte = [](float f) {
            return static_cast<uint32_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return byte(light.r) | (byte(light.g) << 8) | (byte(light.b) << 16) | (255u << 24);
    }

    void SetEntityLight(ShaderHandle shader, const glm::vec3& light) {
        if (!g_renderBackend || shader == INVALID_SHADER) return;
        g_renderBackend->SetUniformVec3(shader, "uEntityLight", light);
    }

    void SetDrawLight(ShaderHandle shader, const glm::vec3& light) {
        if (!g_renderBackend || shader == INVALID_SHADER) return;
        g_renderBackend->SetUniformVec3(shader, "uDrawLight", light);
    }

    void ApplyWorld(ShaderHandle shader, const glm::dvec3& cameraWorld) {
        if (!g_renderBackend || shader == INVALID_SHADER) return;
        const EnvironmentFrame& env = EnvironmentState::Get().Frame();
        g_renderBackend->SetUniformFloat(shader, "uSkyBrightness", env.skyBrightness);
        g_renderBackend->SetUniformVec4(shader, "uFogColor", glm::vec4(env.fogColor, 1.0f));
        g_renderBackend->SetUniformVec4(shader, "uFogEnv",
            glm::vec4(env.fogEnvStart, env.fogEnvEnd, env.fogRdStart, env.fogRdEnd));
        g_renderBackend->SetUniformVec3(shader, "uCameraPos", Render::ToRender(cameraWorld));
    }

    void ApplyUnfogged(ShaderHandle shader, float brightness) {
        if (!g_renderBackend || shader == INVALID_SHADER) return;
        g_renderBackend->SetUniformFloat(shader, "uSkyBrightness", brightness);
        // Strength 0 makes the mix a no-op whatever the distance.
        g_renderBackend->SetUniformVec4(shader, "uFogColor", glm::vec4(0.0f));
        g_renderBackend->SetUniformVec4(shader, "uFogEnv", glm::vec4(1e9f));
        g_renderBackend->SetUniformVec3(shader, "uCameraPos", glm::vec3(0.0f));
    }

} // namespace Render::EntityEnvironment
