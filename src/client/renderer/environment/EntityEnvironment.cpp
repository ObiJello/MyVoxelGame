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

#include <algorithm>
#include <cmath>

namespace Render::EntityEnvironment {

    ShaderHandle CreateShader(const std::string& vertexPath, const std::string& fragmentPath,
                              bool readsCommonMatrices) {
        if (!g_renderBackend) return INVALID_SHADER;
        const ShaderHandle shader = g_renderBackend->CreateShaderFromFilesPortal(vertexPath, fragmentPath);
        if (shader != INVALID_SHADER && !readsCommonMatrices) g_renderBackend->SetShaderIgnoresCommonMatrices(shader);
        return shader;
    }

    float Lit() {
        // Video Settings "Lighting: OFF": fully lit, like the lightmap.
        if (!Lightmap::WorldLightingOn()) return Lightmap::FullBrightValue();
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

    namespace {
        // The coloured light at a cell (ChunkLight's header note), each
        // channel raised to `state`'s channel emission, as a tint on
        // `coords` (LightCoords::WithTint). Only where there is block light.
        int TintedAt(const Client::ClientBlockAccess& blocks, int coords, int x, int y, int z,
                     const Game::BlockState* state) {
            namespace L = Game::Lighting;
            if (L::LightCoords::Block(coords) <= 0) return coords;
            int level[3];
            for (int c = 0; c < 3; ++c) {
                level[c] = blocks.GetBrightness(L::ChannelLayer(c), x, y, z);
                if (state) {
                    level[c] = std::max(level[c], L::BlockLightProperties::ChannelEmission(
                                                      *state, static_cast<L::LightChannel>(c)));
                }
            }
            return L::LightCoords::WithTint(coords, level[0], level[1], level[2]);
        }
    } // namespace

    int PackedLightAt(int x, int y, int z) {
        namespace L = Game::Lighting;
        const Client::ClientBlockAccess* blocks = Client::g_clientBlockAccess;
        if (!blocks) return L::LightCoords::Pack(0, 15);
        const int coords = L::LightCoords::Pack(blocks->GetBrightness(L::LightLayer::Block, x, y, z),
                                                blocks->GetBrightness(L::LightLayer::Sky, x, y, z));
        return TintedAt(*blocks, coords, x, y, z, nullptr);
    }

    int LevelLightCoordsAt(const glm::ivec3& pos) {
        return LevelLightCoordsAt(pos, pos);
    }

    int LevelLightCoordsAt(const glm::ivec3& statePos, const glm::ivec3& lightPos) {
        namespace L = Game::Lighting;
        const Client::ClientBlockAccess* blocks = Client::g_clientBlockAccess;
        if (!blocks) return L::LightCoords::kFullSky;
        const Game::BlockState state = blocks->GetBlockState(statePos.x, statePos.y, statePos.z);
        if (L::BlockLightProperties::EmissiveRendering(state)) return L::LightCoords::kFullBright;
        const int sky = blocks->GetBrightness(L::LightLayer::Sky, lightPos.x, lightPos.y, lightPos.z);
        const int block = std::max(blocks->GetBrightness(L::LightLayer::Block, lightPos.x, lightPos.y, lightPos.z),
                                   L::BlockLightProperties::Emission(state));
        return TintedAt(*blocks, L::LightCoords::Pack(block, sky), lightPos.x, lightPos.y, lightPos.z, &state);
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
        if (!Lightmap::Enabled() || !Lightmap::WorldLightingOn()) {
            return glm::vec3(block >= 15 ? FullBlockLight() : Lit());
        }
        const EnvironmentFrame& frame = EnvironmentState::Get().Frame();
        Lightmap& lightmap = Lightmap::Get();
        const int tint = L::LightCoords::Tint15(packedLight);
        if (tint == 0) return lightmap.SampleFor(frame, block, sky);
        // Coloured light (the terrain shader's terrainLight): one sample per
        // channel at that channel's share of the block light, between the
        // two nearest lightmap columns, keeping that channel.
        const auto sampleAt = [&](float level) {
            const int b0 = std::clamp(static_cast<int>(level), 0, 15);
            const int b1 = std::min(b0 + 1, 15);
            const float t = std::clamp(level - static_cast<float>(b0), 0.0f, 1.0f);
            return glm::mix(lightmap.SampleFor(frame, b0, sky), lightmap.SampleFor(frame, b1, sky), t);
        };
        const glm::vec3 share = glm::vec3(1.0f) - glm::vec3(static_cast<float>(tint & 31),
                                                            static_cast<float>((tint >> 5) & 31),
                                                            static_cast<float>((tint >> 10) & 31)) / 31.0f;
        const glm::vec3 level = static_cast<float>(block) * share;
        return glm::vec3(sampleAt(level.r).r, sampleAt(level.g).g, sampleAt(level.b).b);
    }

    glm::vec3 LightColorCoords(int lightCoords) {
        namespace L = Game::Lighting;
        // Levels with fractions: coords are level * 16 (0..240).
        const float block = std::clamp(static_cast<float>(L::LightCoords::SmoothBlock(lightCoords)) / 16.0f, 0.0f, 15.0f);
        const float sky   = std::clamp(static_cast<float>(L::LightCoords::SmoothSky(lightCoords)) / 16.0f, 0.0f, 15.0f);
        if (!Lightmap::Enabled() || !Lightmap::WorldLightingOn()) {
            return glm::vec3(block >= 15.0f ? FullBlockLight() : Lit());
        }
        const EnvironmentFrame& frame = EnvironmentState::Get().Frame();
        Lightmap& lightmap = Lightmap::Get();
        const int b0 = static_cast<int>(block), s0 = static_cast<int>(sky);
        const int b1 = std::min(b0 + 1, 15), s1 = std::min(s0 + 1, 15);
        const float tb = block - static_cast<float>(b0), ts = sky - static_cast<float>(s0);
        if (tb == 0.0f && ts == 0.0f) return lightmap.SampleFor(frame, b0, s0);
        const glm::vec3 c00 = lightmap.SampleFor(frame, b0, s0), c10 = lightmap.SampleFor(frame, b1, s0);
        const glm::vec3 c01 = lightmap.SampleFor(frame, b0, s1), c11 = lightmap.SampleFor(frame, b1, s1);
        return glm::mix(glm::mix(c00, c10, tb), glm::mix(c01, c11, tb), ts);
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
