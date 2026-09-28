// File: src/client/renderer/environment/WeatherEffectRenderer.cpp
//
// MC WeatherEffectRenderer — see the header.
#include "WeatherEffectRenderer.hpp"

#include "EntityEnvironment.hpp"
#include "EnvironmentState.hpp"
#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../mesh/ChunkRenderer.hpp"   // PortalClipPlane
#include "../particle/MobParticleSystem.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "client/world/ClientLevel.hpp"
#include "client/world/ClientWeather.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/lighting/LightCoords.hpp"
#include "platform/GameDirectory.hpp"
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    WeatherEffectRenderer g_weatherEffectRenderer;

    namespace {

        // Java int arithmetic: wraps on overflow (the column hashes rely on it).
        int32_t Mul(int32_t a, int32_t b) {
            return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
        }
        int32_t Add(int32_t a, int32_t b) {
            return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
        }

        TextureHandle LoadWeatherTexture(const char* relPath) {
            const std::string full = PlatformMain::GetAssetPath(relPath);
            if (!std::filesystem::exists(full)) {
                Log::Warning("WeatherEffectRenderer: missing texture %s", full.c_str());
                return INVALID_TEXTURE;
            }
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("WeatherEffectRenderer: cannot decode %s", full.c_str());
                return INVALID_TEXTURE;
            }
            TextureHandle t = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (t != INVALID_TEXTURE) {
                // The V coordinate runs to y / 4 blocks and scrolls without
                // bound, and snow's U drifts: the texture repeats, nearest.
                g_renderBackend->SetTextureFilter(t, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(t, TextureWrap::Repeat, TextureWrap::Repeat);
            }
            return t;
        }

    } // namespace

    WeatherEffectRenderer::WeatherEffectRenderer() {
        // MC's constructor: for each of the 32 x 32 cells around the camera
        // column, the unit vector perpendicular to the direction to it —
        // (-dz, dx) / |d|. The camera's own cell divides 0 by 0 (NaN, as in
        // Java); Render skips that column, which draws nothing either way.
        for (int z = 0; z < 32; ++z) {
            for (int x = 0; x < 32; ++x) {
                const float deltaX = static_cast<float>(x - 16);
                const float deltaZ = static_cast<float>(z - 16);
                const float distance = std::sqrt(deltaX * deltaX + deltaZ * deltaZ);
                m_columnSizeX[static_cast<size_t>(z * 32 + x)] = -deltaZ / distance;
                m_columnSizeZ[static_cast<size_t>(z * 32 + x)] = deltaX / distance;
            }
        }
    }

    WeatherEffectRenderer::~WeatherEffectRenderer() = default;

    bool WeatherEffectRenderer::Initialize() {
        if (!g_renderBackend) return false;
        // MC's WEATHER pipeline runs the particle shader (PARTICLE_SNIPPET).
        if (g_renderBackend->GetType() == BackendType::OpenGL) {
            m_shader = g_renderBackend->CreateShader(MobParticleSystem::VertexSource(),
                                                     MobParticleSystem::FragmentSource());
        } else {
            m_shader = EntityEnvironment::CreateShader("shaders/mob_particle.vert", "shaders/mob_particle.frag");
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[WeatherEffectRenderer] failed to create the particle shader for %s",
                         g_renderBackend->GetName());
            return false;
        }
        LoadTextures();
        return true;
    }

    void WeatherEffectRenderer::Shutdown() {
        if (!g_renderBackend) return;
        DestroySlots();
        DestroyTextures(/*deferred=*/false);
        if (m_shader != INVALID_SHADER) {
            g_renderBackend->DestroyShader(m_shader);
            m_shader = INVALID_SHADER;
        }
        m_rainColumns.clear();
        m_snowColumns.clear();
        m_vertices.clear();
    }

    void WeatherEffectRenderer::ReloadTextures() {
        if (!g_renderBackend || m_shader == INVALID_SHADER) return;
        LoadTextures();
    }

    void WeatherEffectRenderer::LoadTextures() {
        DestroyTextures(/*deferred=*/true);
        m_rainTexture = LoadWeatherTexture("assets/textures/environment/rain.png");
        m_snowTexture = LoadWeatherTexture("assets/textures/environment/snow.png");
    }

    void WeatherEffectRenderer::DestroyTextures(bool deferred) {
        // Deferred on a reload: a frame still in flight may be sampling the
        // old texture. Immediate at shutdown, with the device going away.
        for (TextureHandle* t : {&m_rainTexture, &m_snowTexture}) {
            if (*t == INVALID_TEXTURE) continue;
            if (deferred) g_renderBackend->DeferredDestroyTexture(*t);
            else          g_renderBackend->DestroyTexture(*t);
            *t = INVALID_TEXTURE;
        }
    }

    WeatherEffectRenderer::StreamSlot& WeatherEffectRenderer::AcquireSlot(size_t vertsNeeded) {
        StreamSlot& slot = m_slots[m_slotCursor];
        m_slotCursor = (m_slotCursor + 1) % kStreamSlots;
        if (slot.vb == INVALID_BUFFER || slot.capacityVerts < vertsNeeded) {
            // 21 x 21 columns x 6 vertices at the widest radius.
            size_t newCap = std::max<size_t>(slot.capacityVerts, 2646);
            while (newCap < vertsNeeded) newCap *= 2;
            if (slot.mesh != INVALID_MESH)  g_renderBackend->DeferredDestroyMesh(slot.mesh);
            if (slot.vb   != INVALID_BUFFER) g_renderBackend->DeferredDestroyBuffer(slot.vb);
            slot.vb   = g_renderBackend->CreateBuffer(BufferUsage::Vertex, newCap * sizeof(Vertex), nullptr,
                                                      BufferAccess::Streaming);
            slot.mesh = g_renderBackend->CreateMesh(slot.vb, INVALID_BUFFER, GetBlockVertexLayout());
            slot.capacityVerts = newCap;
        }
        return slot;
    }

    void WeatherEffectRenderer::DestroySlots() {
        for (StreamSlot& slot : m_slots) {
            if (slot.mesh != INVALID_MESH)  { g_renderBackend->DestroyMesh(slot.mesh);  slot.mesh = INVALID_MESH; }
            if (slot.vb   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(slot.vb); slot.vb = INVALID_BUFFER; }
            slot.capacityVerts = 0;
        }
        m_slotCursor = 0;
    }

    void WeatherEffectRenderer::PrepareInstances(std::vector<Vertex>& out,
                                                 const std::vector<ColumnInstance>& columns,
                                                 const glm::dvec3& cameraPos, float maxAlpha, int radius,
                                                 float intensity) const {
        // MC prepareInstances.
        const float radiusSq = static_cast<float>(radius * radius);
        const int camBlockX = static_cast<int>(std::floor(cameraPos.x));
        const int camBlockZ = static_cast<int>(std::floor(cameraPos.z));
        for (const ColumnInstance& column : columns) {
            const float relativeX = static_cast<float>(static_cast<double>(column.x) + 0.5 - cameraPos.x);
            const float relativeZ = static_cast<float>(static_cast<double>(column.z) + 0.5 - cameraPos.z);
            const float distanceSq = relativeX * relativeX + relativeZ * relativeZ;
            const float lerpT = std::min(distanceSq / radiusSq, 1.0f);
            const float alpha = (maxAlpha + lerpT * (0.5f - maxAlpha)) * intensity;
            const int index = (column.z - camBlockZ + 16) * 32 + column.x - camBlockX + 16;
            if (index < 0 || index >= 1024) continue;
            const float halfSizeX = m_columnSizeX[static_cast<size_t>(index)] / 2.0f;
            const float halfSizeZ = m_columnSizeZ[static_cast<size_t>(index)] / 2.0f;
            // The camera's own column: no direction to face (0 / 0).
            if (std::isnan(halfSizeX) || std::isnan(halfSizeZ)) continue;

            // ARGB.white(alpha): the alpha to 8 bits (floor); the lightmap
            // colour of the column's light in the RGB (the shader's own
            // light is 1 — see the header).
            const int a8 = std::clamp(static_cast<int>(std::floor(alpha * 255.0f)), 0, 255);
            const glm::vec3 light = EntityEnvironment::LightColor(column.lightCoords);
            const uint8_t r = static_cast<uint8_t>(std::clamp(light.r, 0.0f, 1.0f) * 255.0f);
            const uint8_t g = static_cast<uint8_t>(std::clamp(light.g, 0.0f, 1.0f) * 255.0f);
            const uint8_t b = static_cast<uint8_t>(std::clamp(light.b, 0.0f, 1.0f) * 255.0f);
            const uint8_t a = static_cast<uint8_t>(a8);

            // World corners in double, then narrowed through the view's
            // render origin (MC builds them camera-relative in float).
            const double cx = static_cast<double>(column.x) + 0.5;
            const double cz = static_cast<double>(column.z) + 0.5;
            const glm::vec3 p0 = ToRender(glm::dvec3(cx - halfSizeX, static_cast<double>(column.topY),    cz - halfSizeZ));
            const glm::vec3 p1 = ToRender(glm::dvec3(cx + halfSizeX, static_cast<double>(column.topY),    cz + halfSizeZ));
            const glm::vec3 p2 = ToRender(glm::dvec3(cx + halfSizeX, static_cast<double>(column.bottomY), cz + halfSizeZ));
            const glm::vec3 p3 = ToRender(glm::dvec3(cx - halfSizeX, static_cast<double>(column.bottomY), cz - halfSizeZ));
            const float u0 = column.uOffset + 0.0f;
            const float u1 = column.uOffset + 1.0f;
            const float v0 = static_cast<float>(column.bottomY) * 0.25f + column.vOffset;
            const float v1 = static_cast<float>(column.topY) * 0.25f + column.vOffset;
            // MC's quad (x0,y1,z0) (x1,y1,z1) (x1,y0,z1) (x0,y0,z0) with
            // UVs (u0,v0) (u1,v0) (u1,v1) (u0,v1), as two triangles.
            const Vertex q0{p0.x, p0.y, p0.z, u0, v0, r, g, b, a};
            const Vertex q1{p1.x, p1.y, p1.z, u1, v0, r, g, b, a};
            const Vertex q2{p2.x, p2.y, p2.z, u1, v1, r, g, b, a};
            const Vertex q3{p3.x, p3.y, p3.z, u0, v1, r, g, b, a};
            out.push_back(q0); out.push_back(q1); out.push_back(q2);
            out.push_back(q0); out.push_back(q2); out.push_back(q3);
        }
    }

    void WeatherEffectRenderer::Render(const glm::mat4& proj, const glm::mat4& view,
                                       const glm::dvec3& cameraPos, float partialTick) {
        Render(proj, view, cameraPos, partialTick, Client::ClientLevels::BoundDimension(),
               ChunkRenderer::PortalClipPlane());
    }

    void WeatherEffectRenderer::Render(const glm::mat4& proj, const glm::mat4& view,
                                       const glm::dvec3& cameraPos, float partialTick,
                                       Game::DimensionId dimension, const glm::vec4& clipPlane) {
        PROFILE_ZONE_N("WeatherEffects.Render");
        if (m_shader == INVALID_SHADER || !g_renderBackend) return;
        if (!Client::g_clientBlockAccess) return;
        if (!Game::DimensionCanHaveWeather(dimension)) return;

        // ── extractRenderState ──────────────────────────────────────────
        // The view's level's weather: a portal into the Nether shows none.
        const float intensity = Client::ClientWeather::RainLevelIn(dimension, partialTick);
        m_rainColumns.clear();
        m_snowColumns.clear();
        if (!(intensity > 0.0f)) return;

        const int radius = Platform::g_gameSettings.GetWeatherRadius();
        const int cameraBlockX = static_cast<int>(std::floor(cameraPos.x));
        const int cameraBlockY = static_cast<int>(std::floor(cameraPos.y));
        const int cameraBlockZ = static_cast<int>(std::floor(cameraPos.z));
        const int64_t gameTime = EnvironmentState::Get().GameTime();
        const Client::ClientBlockAccess& blocks = *Client::g_clientBlockAccess;
        Game::JavaRandom random(0);

        {
            PROFILE_ZONE_N("WeatherEffects.Extract");
            for (int z = cameraBlockZ - radius; z <= cameraBlockZ + radius; ++z) {
                for (int x = cameraBlockX - radius; x <= cameraBlockX + radius; ++x) {
                    const int terrainHeight = Client::ClientWeather::MotionBlockingHeight(x, z);
                    const int y0 = std::max(cameraBlockY - radius, terrainHeight);
                    const int y1 = std::max(cameraBlockY + radius, terrainHeight);
                    if (y1 - y0 == 0) continue;
                    const int precipitation =
                        Client::ClientWeather::BiomePrecipitationAt(blocks, glm::ivec3(x, cameraBlockY, z));
                    if (precipitation == static_cast<int>(Game::BiomeRegistry::Precipitation::None)) continue;

                    // int seed = x * x * 3121 + x * 45238971 ^ z * z * 418711 + z * 13761;
                    const int32_t seed = Add(Mul(Mul(x, x), 3121), Mul(x, 45238971)) ^
                                         Add(Mul(Mul(z, z), 418711), Mul(z, 13761));
                    random.SetSeed(static_cast<int64_t>(seed));
                    const int lightSampleY = std::max(cameraBlockY, terrainHeight);
                    const int lightCoords = EntityEnvironment::LevelLightCoordsAt(glm::ivec3(x, lightSampleY, z));

                    if (precipitation == static_cast<int>(Game::BiomeRegistry::Precipitation::Rain)) {
                        // createRainColumnInstance.
                        const int wrappedTicks = static_cast<int>(gameTime & 131071LL);
                        const int tickOffset = Add(Add(Add(Mul(Mul(x, x), 3121), Mul(x, 45238971)),
                                                       Mul(Mul(z, z), 418711)), Mul(z, 13761)) & 255;
                        const float blockPosRainSpeed = 3.0f + random.NextFloat();
                        const float textureOffset =
                            -(static_cast<float>(wrappedTicks + tickOffset) + partialTick) / 32.0f * blockPosRainSpeed;
                        const float wrappedTextureOffset = std::fmod(textureOffset, 32.0f);
                        m_rainColumns.push_back({x, z, y0, y1, 0.0f, wrappedTextureOffset, lightCoords});
                    } else {
                        // createSnowColumnInstance.
                        const int wrappedTicks = static_cast<int>(gameTime & 131071LL);
                        const float time = static_cast<float>(wrappedTicks) + partialTick;
                        const double ud = random.NextDouble();
                        const float ug = static_cast<float>(random.NextGaussian());
                        const float u = static_cast<float>(ud + static_cast<double>(time * 0.01f * ug));
                        const double vd = random.NextDouble();
                        const float vg = static_cast<float>(random.NextGaussian());
                        const float v = static_cast<float>(vd + static_cast<double>(time * vg * 0.001f));
                        const float vOffset = -(static_cast<float>(gameTime & 511LL) + partialTick) / 512.0f;
                        namespace LC = Game::Lighting::LightCoords;
                        const int brightened = LC::Pack((LC::Block(lightCoords) * 3 + 15) / 4,
                                                        (LC::Sky(lightCoords) * 3 + 15) / 4);
                        m_snowColumns.push_back({x, z, y0, y1, u, vOffset + v, brightened});
                    }
                }
            }
        }
        if (m_rainColumns.empty() && m_snowColumns.empty()) return;

        // ── prepare ─────────────────────────────────────────────────────
        m_vertices.clear();
        PrepareInstances(m_vertices, m_rainColumns, cameraPos, 1.0f, radius, intensity);
        const size_t rainVerts = m_vertices.size();
        PrepareInstances(m_vertices, m_snowColumns, cameraPos, 0.8f, radius, intensity);
        const size_t snowVerts = m_vertices.size() - rainVerts;
        if (m_vertices.empty()) return;

        StreamSlot& slot = AcquireSlot(m_vertices.size());
        g_renderBackend->UpdateBufferStreaming(slot.vb, 0, m_vertices.size() * sizeof(Vertex), m_vertices.data());

        // ── render (RenderPipelines.WEATHER) ────────────────────────────
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", proj * view);
        g_renderBackend->SetUniformInt(m_shader, "uSprite", 0);
        EntityEnvironment::ApplyWorld(m_shader, cameraPos);
        EntityEnvironment::SetEntityLight(m_shader, glm::vec3(1.0f));
        // The view's clip plane (a portal view's far side only); set every
        // draw — on Vulkan it rides a push-constant slot others write.
        g_renderBackend->SetUniformVec4(m_shader, "uEntityClipPlane", clipPlane);

        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = false;
        s.colorWriteEnabled = true;
        s.blendEnabled      = true;
        s.srcBlendFactor    = BlendFactor::SrcAlpha;
        s.dstBlendFactor    = BlendFactor::OneMinusSrcAlpha;
        s.cullMode          = CullMode::None;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);

        if (rainVerts > 0 && m_rainTexture != INVALID_TEXTURE) {
            g_renderBackend->BindTexture(m_rainTexture, 0);
            g_renderBackend->DrawArrays(slot.mesh, static_cast<uint32_t>(rainVerts), 0);
        }
        if (snowVerts > 0 && m_snowTexture != INVALID_TEXTURE) {
            g_renderBackend->BindTexture(m_snowTexture, 0);
            g_renderBackend->DrawArrays(slot.mesh, static_cast<uint32_t>(snowVerts),
                                        static_cast<uint32_t>(rainVerts));
        }
        g_renderBackend->UnbindMesh();

        PipelineState defaultState;
        defaultState.depthTestEnabled  = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled      = false;
        defaultState.cullMode          = CullMode::Back;
        g_renderBackend->SetPipelineState(defaultState);
    }

} // namespace Render
