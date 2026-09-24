// File: src/client/renderer/blockentity/AurelithQuestRenderers.cpp
//
// See AurelithQuestRenderers.hpp.
#include "AurelithQuestRenderers.hpp"
#include "BlockEntityShader.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/block/AurelithQuestBlocks.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/entity/AurelithBlockEntities.hpp"
#include "client/world/AurelithState.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "client/world/ClientLevel.hpp"
#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../environment/EntityEnvironment.hpp"
#include "../environment/EnvironmentState.hpp"
#include "../viewmodel/HeldItemSpriteMesh.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <string_view>

namespace Render {

    using namespace Aurelith;
    namespace A = Game::Aurelith;

    namespace {

        Rgb VoiceRgb(int voice) {
            if (voice < 0 || voice >= A::kVoiceCount) return Rgb{230, 255, 255};
            const uint32_t c = A::VoiceColour(static_cast<A::Voice>(voice));
            return Rgb{static_cast<uint8_t>((c >> 16) & 0xFF), static_cast<uint8_t>((c >> 8) & 0xFF),
                       static_cast<uint8_t>(c & 0xFF)};
        }

        int VoiceIndexOf(const Game::ItemStack& stack) {
            const std::optional<A::Voice> v = A::VoiceOfKey(stack.itemId);
            return v ? static_cast<int>(*v) : -1;
        }

        // The sprite's yaw so its face (+z) looks out of the socket's front.
        float FacingYaw(std::string_view facing) {
            if (facing == "north") return glm::radians(180.0f);
            if (facing == "east")  return glm::radians(90.0f);
            if (facing == "west")  return glm::radians(-90.0f);
            return 0.0f;                                        // south
        }

        // Place a 0..16 sprite so its centre is at the origin, `size` blocks
        // across.
        glm::mat4 SpriteLocal(float size) {
            glm::mat4 m = glm::scale(glm::mat4(1.0f), glm::vec3(size / 16.0f));
            return glm::translate(m, glm::vec3(-8.0f, -8.0f, 0.0f));
        }

    } // namespace

    // ── the shared half ──────────────────────────────────────────────────

    AurelithQuestRendererBase::~AurelithQuestRendererBase() { Shutdown(); }

    bool AurelithQuestRendererBase::Initialize() {
        if (!g_renderBackend) return false;
        if (m_initialized) return true;
        m_shader = BlockEntityShader::Create();
        if (m_shader == INVALID_SHADER) {
            Log::Error("[AurelithQuestRenderers] shader compile failed");
            return false;
        }
        for (int i = 0; i < 5; ++i) {
            std::vector<Vert> v; std::vector<uint32_t> ix;
            AppendBillboard(v, ix, VoiceRgb(i < 4 ? i : -1), 0.9);
            if (!BuildMesh(m_glow[static_cast<size_t>(i)], v, ix)) {
                Log::Error("[AurelithQuestRenderers] glow mesh creation failed");
                Shutdown();
                return false;
            }
        }
        m_glowTex = MakeTexture(64, 64, GlowPixels(64));
        if (m_glowTex == INVALID_TEXTURE) {
            Log::Error("[AurelithQuestRenderers] glow texture creation failed");
            Shutdown();
            return false;
        }
        m_initialized = true;
        return true;
    }

    void AurelithQuestRendererBase::Shutdown() {
        if (!g_renderBackend) return;
        for (auto& m : m_glow) DestroyMesh(m);
        if (m_glowTex != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_glowTex); m_glowTex = INVALID_TEXTURE; }
        if (m_shader != INVALID_SHADER)   { g_renderBackend->DestroyShader(m_shader);   m_shader = INVALID_SHADER; }
        // The sprite meshes belong to HeldItemSpriteMesh's shared cache.
        m_initialized = false;
    }

    bool AurelithQuestRendererBase::DrawItemSprite(const Game::ItemStack& stack, const glm::mat4& model,
                                                   float light, const glm::mat4& proj,
                                                   const glm::mat4& view, const glm::vec3& cameraPos) {
        if (stack.IsEmpty()) return false;
        const auto* entry = HeldItemSpriteMesh::GetOrBuildForStack(stack);
        if (!entry || entry->mesh == INVALID_MESH) return false;
        PipelineState s = SolidPipeline();
        g_renderBackend->SetPipelineState(s);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.5f);
        g_renderBackend->BindTexture(entry->texture, 0);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", proj * view * model);
        BlockEntityShader::ApplyWorld(m_shader, model, cameraPos);
        BlockEntityShader::SetLight(m_shader, light);
        g_renderBackend->DrawIndexed(entry->mesh, entry->indexCount);
        return true;
    }

    void AurelithQuestRendererBase::DrawGlow(int voice, const glm::vec3& at, float size, float light,
                                             const glm::mat4& proj, const glm::mat4& view,
                                             const glm::vec3& cameraPos) {
        const auto& mesh = m_glow[static_cast<size_t>(voice >= 0 && voice < 4 ? voice : 4)];
        if (!mesh.Valid() || light <= 0.002f) return;
        g_renderBackend->SetPipelineState(GlowPipeline());
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.0f);
        g_renderBackend->BindTexture(m_glowTex, 0);
        const glm::mat4 m = BillboardModel(view, at, size);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", proj * view * m);
        BlockEntityShader::ApplyWorld(m_shader, m, cameraPos);
        BlockEntityShader::SetLight(m_shader, light);
        g_renderBackend->DrawIndexed(mesh.mesh, mesh.indexCount);
    }

    // ── the chord socket ─────────────────────────────────────────────────

    void ChordSocketRenderer::Render(const Game::BlockEntity& be, float partialTick,
                                     const glm::mat4& proj, const glm::mat4& view,
                                     const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("BE.ChordSocket");
        if (!m_initialized || !g_renderBackend) return;
        const auto* socket = dynamic_cast<const Game::ChordSocketBlockEntity*>(&be);
        if (!socket || !socket->HasKey()) return;

        const glm::ivec3 pos = be.GetWorldPos();
        const int voice = VoiceIndexOf(socket->GetKey());
        const double ticks = EnvironmentState::Get().GameTimeF(partialTick);
        const uint64_t seed = Mix64(PositionSeed(pos));

        std::string_view facing = "south";
        if (Client::g_clientBlockAccess) {
            const Game::BlockState state = Client::g_clientBlockAccess->GetBlockState(pos.x, pos.y, pos.z);
            if (state.Block() == be.GetBlockId()) facing = state.GetValueByName("facing");
        }

        // How brightly this voice sings now.
        double glow = 0.30 + 0.08 * std::sin(ticks / 20.0 * 1.3 + kTwoPi * Draw(seed, 0, 1));
        double flare = 0.0;
        const auto city = Client::AurelithState::Nearest(Client::ClientLevels::ActiveDimension(),
                                                         glm::dvec3(pos) + glm::dvec3(0.5), 32.0);
        if (city && city->state != A::CityState::Dormant) {
            const double t = Client::AurelithState::StageTicks(*city, ticks);
            const double voiceLevel = Client::AurelithState::Voice(*city, ticks);
            const double sour = Client::AurelithState::Sourness(*city, ticks);
            glow = 0.55 + 0.45 * voiceLevel;
            if (city->state == A::CityState::Awakening) {
                // Its own note of the arpeggio, then the Chord.
                int order = 0;
                for (int i = 0; i < A::kVoiceCount; ++i) {
                    if (static_cast<int>(A::kChordOrder[i]) == voice) order = i;
                }
                const double own = (t - order * A::kArpeggioStep) / 5.0;
                const double chord = (t - A::kChordStruck) / 9.0;
                flare = 1.4 * std::exp(-own * own) + 1.1 * std::exp(-chord * chord);
            }
            // The Undersong: the voices gutter against it.
            glow *= 1.0 - 0.45 * sour * (0.5 + 0.5 * ValueNoise(seed, ticks * 0.35, 7));
        }

        const bool locked = socket->IsLocked();
        const double bob = locked ? 0.0 : 0.015 * std::sin(ticks / 20.0 * 2.1 + kTwoPi * Draw(seed, 0, 2));
        const glm::vec3 base = ToRender(glm::dvec3(pos) + glm::dvec3(0.5, 1.02 + bob, 0.5));

        // The key stood in the keyway: bow up, bit down, its face out of the
        // socket's front (toward the Heart).
        glm::mat4 model = glm::translate(glm::mat4(1.0f), base);
        model = glm::rotate(model, FacingYaw(facing), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        model = model * SpriteLocal(0.62f);
        const float keyLight = static_cast<float>(EntityEnvironment::kEmissive * std::min(1.0, 0.7 + 0.3 * glow + 0.3 * flare));
        DrawItemSprite(socket->GetKey(), model, keyLight, proj, view, cameraPos);

        // The voice's glow behind it, and on a flare a wider bloom.
        DrawGlow(voice, base, 0.9f + 0.5f * static_cast<float>(flare),
                 static_cast<float>(0.45 * glow + 0.6 * flare), proj, view, cameraPos);
        if (locked || flare > 0.05) {
            DrawGlow(voice, base + glm::vec3(0.0f, 0.35f, 0.0f), 2.2f + 1.5f * static_cast<float>(flare),
                     static_cast<float>(0.18 * glow + 0.35 * flare), proj, view, cameraPos);
        }
        g_renderBackend->UnbindMesh();
    }

    // ── the voice pedestal ───────────────────────────────────────────────

    void VoicePedestalRenderer::Render(const Game::BlockEntity& be, float partialTick,
                                       const glm::mat4& proj, const glm::mat4& view,
                                       const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("BE.VoicePedestal");
        if (!m_initialized || !g_renderBackend) return;
        const auto* pedestal = dynamic_cast<const Game::VoicePedestalBlockEntity*>(&be);
        if (!pedestal || !pedestal->HasItem()) return;

        const glm::ivec3 pos = be.GetWorldPos();
        const double ticks = EnvironmentState::Get().GameTimeF(partialTick);
        const uint64_t seed = Mix64(PositionSeed(pos));
        const int voice = VoiceIndexOf(pedestal->GetItem());

        // Turning a full circle every ~9 s, bobbing on a slower breath.
        const double spin = ticks / 20.0 * (kTwoPi / 9.0) + kTwoPi * Draw(seed, 0, 1);
        const double bob = 0.06 * std::sin(ticks / 20.0 * 1.4 + kTwoPi * Draw(seed, 0, 2));
        const glm::vec3 at = ToRender(glm::dvec3(pos) + glm::dvec3(0.5, 1.30 + bob, 0.5));

        glm::mat4 model = glm::translate(glm::mat4(1.0f), at);
        model = glm::rotate(model, static_cast<float>(spin), glm::vec3(0.0f, 1.0f, 0.0f));
        if (voice >= 0) model = glm::rotate(model, glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        model = model * SpriteLocal(0.55f);
        const float light = voice >= 0 ? EntityEnvironment::kEmissive : EntityEnvironment::FullBlockLight();
        DrawItemSprite(pedestal->GetItem(), model, light, proj, view, cameraPos);

        const double breath = 0.75 + 0.25 * std::sin(ticks / 20.0 * 1.4 + kTwoPi * Draw(seed, 0, 2));
        DrawGlow(voice, at, voice >= 0 ? 1.1f : 0.7f,
                 static_cast<float>((voice >= 0 ? 0.55 : 0.2) * breath), proj, view, cameraPos);
        if (voice >= 0) {
            DrawGlow(voice, at - glm::vec3(0.0f, 0.25f, 0.0f), 2.6f,
                     static_cast<float>(0.16 * breath), proj, view, cameraPos);
        }
        g_renderBackend->UnbindMesh();
    }

} // namespace Render
