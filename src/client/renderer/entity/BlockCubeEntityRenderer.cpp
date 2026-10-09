// File: src/client/renderer/entity/BlockCubeEntityRenderer.cpp
#include "../mesh/ChunkRenderer.hpp"
#include "BlockCubeEntityRenderer.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "EntityCulling.hpp"
#include "../core/Frustum.hpp"
#include "../core/RenderOrigin.hpp"

#include "../backend/RenderBackend.hpp"
#include "../environment/EnvironmentState.hpp"
#include "../environment/EntityEnvironment.hpp"
#include "../environment/Lightmap.hpp"
#include "common/world/lighting/BlockLightProperties.hpp"
#include "common/world/lighting/ChunkLight.hpp"
#include "common/world/lighting/LightCoords.hpp"
#include "../mesh/BlockModelLighter.hpp"
#include "../mesh/BlockTint.hpp"
#include "client/world/ClientLevel.hpp"
#include "../mesh/Mesher.hpp"
#include <cstddef>
#include "../texture/AtlasBuilder.hpp"
#include "../viewmodel/ItemMeshBuilder.hpp"
#include "client/entity/ClientMobManager.hpp"
#include "client/entity/ClientFallingBlocks.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/core/TickParallel.hpp"
#include "common/entity/FallingBlockEntity.hpp"
#include "common/entity/PrimedTnt.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"


#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <cmath>
#include <cstring>

namespace Render {

    BlockCubeEntityRenderer g_blockCubeEntityRenderer;

    namespace {

        // ── The primed-TNT white flash, derived from MC's OverlayTexture ────
        //
        // MC does NOT tint the TNT white. TntRenderer calls
        // TntMinecartRenderer.submitWhiteSolidBlock(..., white=true, ...),
        // which sets the overlay texture coordinate to
        // `OverlayTexture.pack(OverlayTexture.u(1.0F), 10)` — that is
        // (u=15, v=10) — and the entity shader then does
        //
        //     color.rgb = mix(overlayColor.rgb, color.rgb, overlayColor.a);
        //
        // Row v=10 of that 16x16 texture is built as
        //
        //     a = (int)((1.0F - x / 15.0F * 0.75F) * 255.0F);  pixel = white(a)
        //
        // so at x=15 the texel is WHITE with alpha (1 - 0.75) * 255 = 63.
        // Feeding that through the mix gives
        //
        //     0.753 * white + 0.247 * blockColour
        //
        // — about three-quarters of the way to white, NOT a full whiteout. The
        // block's texture still shows through, which is exactly why vanilla TNT
        // reads as "glowing" rather than as a blank white cube.
        //
        // The strength here is 1 - 63/255 because this engine's uniform carries
        // the alpha inverted so that an unset vec4(0) is a passthrough; see the
        // note in shaders/block.frag.
        constexpr float kTntFlashTexelAlpha = 63.0f / 255.0f;
        const glm::vec4 kTntFlashOverlay(1.0f, 1.0f, 1.0f,
                                         1.0f - kTntFlashTexelAlpha);

        // BlockModelLighter's view of the live client level — the level the
        // section mesher snapshots — for lighting a moving block the way the
        // section mesh lights the same block at rest (Mesher::LighterLevel
        // is the snapshot twin).
        struct ClientLevelLighter {
            const Client::ClientBlockAccess& level;
            Game::BlockState StateAt(int x, int y, int z) const { return level.GetBlockState(x, y, z); }
            // MC LightCoordsUtil.getLightCoords(state, level, pos), as
            // Mesher::LightCoordsWith reads it from the snapshot.
            int LightCoordsWith(Game::BlockState s, int x, int y, int z) const {
                namespace L = Game::Lighting;
                if (L::BlockLightProperties::EmissiveRendering(s)) return L::LightCoords::kFullBright;
                const int sky = level.GetBrightness(L::LightLayer::Sky, x, y, z);
                const int block = std::max(level.GetBrightness(L::LightLayer::Block, x, y, z),
                                           L::BlockLightProperties::Emission(s));
                return L::LightCoords::Pack(block, sky);
            }
            bool LightPermeableAt(int x, int y, int z) const {
                return Game::Lighting::BlockLightProperties::LightPermeable(level.GetBlockState(x, y, z));
            }
            float ShadeAt(int x, int y, int z) const {
                return Game::Lighting::BlockLightProperties::ShadeBrightness(level.GetBlockState(x, y, z));
            }
        };

        // MC MovingBlockRenderState as a BlockModelLighter level, for the
        // part of a falling block's lighting that does not depend on where
        // it is: getBlockState answers the carried state at blockPos (the
        // origin here) and AIR everywhere else, so every neighbour lets
        // light through and shades 1.0 — the ambient occlusion comes out of
        // the model alone. (Its light is the level's, read per instance:
        // FillFallingLight.)
        struct MovingBlockView {
            Game::BlockState state;
            Game::BlockState StateAt(int x, int y, int z) const {
                return (x == 0 && y == 0 && z == 0) ? state : Game::BlockState{};
            }
            bool LightPermeableAt(int x, int y, int z) const {
                return Game::Lighting::BlockLightProperties::LightPermeable(StateAt(x, y, z));
            }
            float ShadeAt(int x, int y, int z) const {
                return Game::Lighting::BlockLightProperties::ShadeBrightness(StateAt(x, y, z));
            }
        };

        // MC packed light coords -> an instance cell byte (block | sky << 4).
        uint8_t PackCell(int packed) {
            namespace LC = Game::Lighting::LightCoords;
            return static_cast<uint8_t>(LC::Block(packed) | (LC::Sky(packed) << 4));
        }

        // A vertex's light recipe (block_instanced.vert, the vertex alpha).
        constexpr uint8_t kRecipeSmooth   = 0x80;   // | faceCubic << 3 | direction
        constexpr uint8_t kRecipeCubicBit = 0x08;
        constexpr uint8_t kRecipeOwnCell  = 6;      // flat: the light cell itself

        // A falling block's shared mesh is keyed apart from primed TNT's of
        // the same state: the two are coloured differently (see DrawItem).
        constexpr uint32_t kFallingMeshBit = 0x40000000u;

        Game::Direction ToDirection(Game::FaceDir d) {
            switch (d) {
                case Game::FaceDir::Up:    return Game::Direction::Up;
                case Game::FaceDir::Down:  return Game::Direction::Down;
                case Game::FaceDir::North: return Game::Direction::North;
                case Game::FaceDir::South: return Game::Direction::South;
                case Game::FaceDir::West:  return Game::Direction::West;
                case Game::FaceDir::East:  return Game::Direction::East;
            }
            return Game::Direction::Up;
        }

        // The bound level's cardinal lighting (MC ClientLevel.cardinalLighting,
        // which PistonHeadRenderer / FallingBlockRenderer copy into the
        // MovingBlockRenderState) — the dimension the section mesher builds
        // this level's meshes for.
        bool BoundLevelNetherCardinalLight() {
            return Game::UsesNetherCardinalLight(Client::ClientLevels::BoundDimension());
        }

        // Each quad's colour before light: MC putQuadWithTint's tint
        // (BlockTint::ColorInWorld — the mesher's dispatch) times
        // getDirectionalBrightness (Game::ElementShade — the mesher's
        // shade), in 0..1. `biomeColor(Channel)` is the biome colour the
        // caller's level answers for the block.
        template <class BiomeColor>
        void QuadBaseColors(Game::BlockState state, const std::vector<BlockModelQuad>& quads,
                            bool netherCardinalLight, BiomeColor&& biomeColor,
                            std::vector<glm::vec3>& out) {
            const BlockTint::Profile& profile = BlockTint::ProfileOf(state.Block());
            // MC ModelBlockRenderer's tint cache: one colour per tint index
            // per block (the biome colour is asked at most once a channel).
            int      cachedIndex = -2;
            uint32_t cachedTint  = BlockTint::kUntinted;
            out.resize(quads.size());
            for (size_t i = 0; i < quads.size(); ++i) {
                const BlockModelQuad& q = quads[i];
                if (q.tintIndex != cachedIndex) {
                    cachedIndex = q.tintIndex;
                    cachedTint = BlockTint::ColorInWorld(profile, q.tintIndex, state, biomeColor);
                }
                const float shade = Game::ElementShade(q.dir, q.shade, netherCardinalLight);
                out[i] = glm::vec3(static_cast<float>((cachedTint >> 16) & 0xFF),
                                   static_cast<float>((cachedTint >> 8) & 0xFF),
                                   static_cast<float>(cachedTint & 0xFF)) * (shade / 255.0f);
            }
        }

        uint8_t ToByte(float v) {
            return static_cast<uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
        }

    } // namespace

    bool BlockCubeEntityRenderer::Initialize() {
        if (!g_renderBackend) return false;

        // Same block shader ItemEntityRenderer uses, and the same Vulkan
        // caveat: the block shaders declare the Common UBO, so on VK they must
        // be created through the UBO-aware (portal) layout or pipeline
        // creation fails.
        m_shader = g_renderBackend->CreateShaderFromFilesPortal("shaders/block.vert", "shaders/block.frag");
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[BlockCubeEntityRenderer] failed to load block shader — "
                         "falling blocks and TNT will not render");
            return false;
        }

        for (int slot = 0; slot < EntityFrame::Slots(); ++slot) {
            FrameBuffers& fb = m_cubeFrames[slot];
            fb.vb = g_renderBackend->CreateBuffer(
                BufferUsage::Vertex, kItemCubeMaxVerts * sizeof(ItemCubeVert),
                nullptr, BufferAccess::Streaming);
            fb.ib = g_renderBackend->CreateBuffer(
                BufferUsage::Index, kItemCubeMaxIdx * sizeof(uint32_t),
                nullptr, BufferAccess::Streaming);
            fb.mesh = g_renderBackend->CreateMesh(fb.vb, fb.ib, GetBlockVertexLayout());
        }

        // ── The instanced path, if the backend has one ─────────────────────
        //
        // Every failure below is non-fatal and leaves instMesh invalid, which
        // Render reads as "use the per-entity loop". That is why the shader is
        // loaded through the plain path and not the Vulkan portal path: Vulkan
        // has no CreateInstancedMesh, so it never gets here anyway.
        {
            // Portal layout, like m_shader: block_vk.frag reads the
            // fog/environment CommonUBO at set=1. Resolves to
            // block_instanced_vk.vert.spv + block_vk.frag.spv.
            m_instShader = g_renderBackend->CreateShaderFromFilesPortal(
                "shaders/block_instanced.vert", "shaders/block.frag");

            if (m_instShader != INVALID_SHADER) {
                for (int slot = 0; slot < EntityFrame::Slots(); ++slot) {
                    FrameBuffers& fb = m_cubeFrames[slot];
                    fb.inst = g_renderBackend->CreateBuffer(
                        BufferUsage::Vertex, kInitialInstances * sizeof(Instance),
                        nullptr, BufferAccess::Streaming);
                    fb.instCapacity = fb.inst != INVALID_BUFFER ? kInitialInstances : 0;
                }
            }

            // Every live set or none (see below).
            auto allSets = [this](auto pred) {
                for (int slot = 0; slot < EntityFrame::Slots(); ++slot) if (!pred(m_cubeFrames[slot])) return false;
                return true;
            };
            if (allSets([](const FrameBuffers& fb) { return fb.inst != INVALID_BUFFER; })) {
                // Location 3: one vec4 per instance — xyz the world
                // translation, w the uniform scale (see Instance). The
                // divisor of 1 is what makes it per-instance. Locations 4-7:
                // the instance's 27 cell lights and flags, as unsigned
                // shorts (two cells each), not normalized — the shader
                // splits the bytes.
                VertexLayout instanceLayout;
                instanceLayout.stride = sizeof(Instance);
                {
                    VertexAttribute attr;
                    attr.location        = 3;
                    attr.componentCount  = 4;
                    attr.offset          = 0;
                    attr.type            = AttribType::Float;
                    attr.instanceDivisor = 1;
                    instanceLayout.attributes.push_back(attr);
                }
                for (uint32_t k = 0; k < 4; ++k) {
                    VertexAttribute attr;
                    attr.location        = 4 + k;
                    attr.componentCount  = k < 3 ? 4 : 2;   // 8 + 8 + 8 + 4 bytes = cells[28]
                    attr.offset          = static_cast<uint32_t>(offsetof(Instance, cells)) + k * 8;
                    attr.normalized      = false;
                    attr.type            = AttribType::UShort;
                    attr.instanceDivisor = 1;
                    instanceLayout.attributes.push_back(attr);
                }

                // Vulkan / Metal pipelines bake the vertex input, so the
                // shader must know both the per-vertex and per-instance
                // layouts.
                g_renderBackend->RegisterShaderVertexLayout(m_instShader, GetBlockVertexLayout());
                g_renderBackend->RegisterShaderInstanceLayout(m_instShader, instanceLayout);
                m_instanceLayout = instanceLayout;
                for (int slot = 0; slot < EntityFrame::Slots(); ++slot) {
                    FrameBuffers& fb = m_cubeFrames[slot];
                    fb.instMesh = g_renderBackend->CreateInstancedMesh(
                        fb.vb, fb.ib, fb.inst,
                        GetBlockVertexLayout(), instanceLayout);
                }
                // Both or neither: a set without its instanced mesh would
                // flip paths mid-frame, and the two are not interchangeable
                // within a pass (see Render's useInstanced).
                if (!allSets([](const FrameBuffers& fb) { return fb.instMesh != INVALID_MESH; })) {
                    for (FrameBuffers& fb : m_cubeFrames) {
                        if (fb.instMesh != INVALID_MESH) {
                            g_renderBackend->DestroyMesh(fb.instMesh);
                            fb.instMesh = INVALID_MESH;
                        }
                    }
                }
            }

            if (m_cubeFrames[0].instMesh == INVALID_MESH) {
                Log::Info("[BlockCubeEntityRenderer] no instanced path — falling "
                          "back to one draw per entity");
            }
        }

        m_initialized = true;
        Log::Info("[BlockCubeEntityRenderer] initialized");
        return true;
    }

    bool BlockCubeEntityRenderer::EnsureInstanceCapacity(FrameBuffers& fb, size_t needed) {
        if (needed <= fb.instCapacity) return true;
        if (fb.instCapacity >= kMaxInstances) return false;
        size_t capacity = std::max<size_t>(fb.instCapacity, kInitialInstances);
        while (capacity < needed && capacity < kMaxInstances) capacity *= 2;
        capacity = std::min(capacity, kMaxInstances);
        if (!ResizeInstanceBuffer(fb, capacity)) return false;
        m_instCursor = 0;
        return true;
    }

    bool BlockCubeEntityRenderer::ResizeInstanceBuffer(FrameBuffers& fb, size_t capacity) {
        const BufferHandle buffer = g_renderBackend->CreateBuffer(
            BufferUsage::Vertex, capacity * sizeof(Instance), nullptr, BufferAccess::Streaming);
        if (buffer == INVALID_BUFFER) return false;
        const MeshHandle mesh = g_renderBackend->CreateInstancedMesh(
            fb.vb, fb.ib, buffer, GetBlockVertexLayout(), m_instanceLayout);
        if (mesh == INVALID_MESH) {
            g_renderBackend->DestroyBuffer(buffer);
            return false;
        }
        // Earlier calls this frame drew from the old pair; it lives until the
        // GPU is done with it.
        if (fb.instMesh != INVALID_MESH) g_renderBackend->DeferredDestroyMesh(fb.instMesh);
        if (fb.inst != INVALID_BUFFER)   g_renderBackend->DeferredDestroyBuffer(fb.inst);
        fb.inst         = buffer;
        fb.instMesh     = mesh;
        fb.instCapacity = capacity;
        return true;
    }

    void BlockCubeEntityRenderer::MaybeShrinkInstanceBuffer(FrameBuffers& fb) {
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const size_t used = fb.instFrameUsed;
        fb.instFrameUsed = 0;
        if (fb.instCapacity <= kInitialInstances || fb.instMesh == INVALID_MESH) {
            fb.instLowPeak = 0;
            fb.instLowSince = now;
            return;
        }
        if (used > fb.instCapacity / 4) {
            // Still busy: the low-use stretch starts over.
            fb.instLowPeak = 0;
            fb.instLowSince = now;
            return;
        }
        fb.instLowPeak = std::max(fb.instLowPeak, used);
        if (now - fb.instLowSince < kInstanceShrinkSeconds) return;

        size_t capacity = kInitialInstances;
        while (capacity < fb.instLowPeak * 2 && capacity < fb.instCapacity) capacity *= 2;
        if (capacity < fb.instCapacity) ResizeInstanceBuffer(fb, capacity);
        fb.instLowPeak = 0;
        fb.instLowSince = now;
    }

    void BlockCubeEntityRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (FrameBuffers& fb : m_cubeFrames) {
            if (fb.instMesh != INVALID_MESH) { g_renderBackend->DestroyMesh(fb.instMesh); fb.instMesh = INVALID_MESH; }
        }
        for (FrameBuffers& fb : m_cubeFrames) {
            if (fb.inst != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(fb.inst); fb.inst = INVALID_BUFFER; }
        }
        if (m_instShader != INVALID_SHADER){ g_renderBackend->DestroyShader(m_instShader); m_instShader = INVALID_SHADER; }
        for (FrameBuffers& fb : m_cubeFrames) {
            if (fb.mesh != INVALID_MESH)  { g_renderBackend->DestroyMesh(fb.mesh); fb.mesh = INVALID_MESH; }
            if (fb.vb   != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(fb.vb); fb.vb = INVALID_BUFFER; }
            if (fb.ib   != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(fb.ib); fb.ib = INVALID_BUFFER; }
        }
        if (m_shader   != INVALID_SHADER){ g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_initialized = false;
    }

    void BlockCubeEntityRenderer::SetRenderDistanceChunks(int chunks, float entityDistanceScaling) {
        // chunks * 4 blocks — a quarter of the render distance, matching
        // ItemEntityRenderer. See its note for why this deliberately diverges
        // from MC's shouldRenderAtSqrDistance. The Entity Distance option
        // multiplies it, as there.
        m_cullRadius = static_cast<float>(std::max(chunks, 2) * 4) * entityDistanceScaling;
    }

    void BlockCubeEntityRenderer::Render(const glm::mat4& projection,
                                         const glm::mat4& view,
                                         const glm::vec3& cameraPos,
                                         float partialTick,
                                         bool movingBlocksOnly) {
        if (!m_initialized || !g_renderBackend || !Client::g_clientMobManager) return;
        PROFILE_ZONE_N("BlockCubeRender");

        const glm::mat4 viewProj = projection * view;
        const float cullSq = m_cullRadius * m_cullRadius;

        // MC EntityRenderDispatcher -> EntityRenderer.shouldRender:62-74.
        // Distance alone was never MC's test; it also asks the frustum, which
        // is what stops a thousand primed TNT behind the camera from each
        // costing a draw, a matrix and a uniform slot. On Vulkan those slots
        // are finite, so this is a correctness contributor as well as a
        // performance one. Culling stays in WORLD space: `view` is the
        // render-space (camera-relative) view, so the frustum is built from
        // its world twin.
        const Frustum frustum = Frustum::FromMatrix(projection * Render::WorldViewFromRenderView(view));

        // Which streaming set this call writes, and where in it — see
        // EntityFrame.hpp. Decided up front so useInstanced can name the
        // set's instanced mesh.
        const bool newFrame = m_frameCursor.Advance();
        if (newFrame) {
            m_vertCursor = 0;
            m_idxCursor  = 0;
            m_instCursor = 0;
        }
        FrameBuffers& fb = m_cubeFrames[m_frameCursor.slot];
        // The slot was last written two frames ago; nothing this frame has
        // drawn from it yet, so this is where it may shrink.
        if (newFrame) MaybeShrinkInstanceBuffer(fb);
        if (fb.mesh == INVALID_MESH) return;

        // The client's own block view, for the shouldRender guard below.
        const Game::IBlockAccess* blocks =
            Client::g_clientMobManager->Level().Blocks();

        std::vector<ItemCubeVert> verts;
        std::vector<uint32_t>     idx;

        // One glDrawElementsInstanced per (block state, flash state) instead of
        // one glDrawElements per entity, whenever the backend can. Decided once
        // per frame so beginPass binds the matching shader — the two take the
        // model matrix by different routes (uMVP uniform vs per-instance
        // attribute) and are not interchangeable mid-pass.
        const bool useInstanced =
            fb.instMesh != INVALID_MESH && m_instShader != INVALID_SHADER;
        const ShaderHandle shader = useInstanced ? m_instShader : m_shader;

        bool anyDrawn = false;
        auto beginPass = [&]() {
            if (anyDrawn) return;
            anyDrawn = true;

            PipelineState state;
            state.depthTestEnabled  = true;
            state.depthWriteEnabled = true;
            state.blendEnabled      = false;
            state.cullMode   = CullMode::Back;
            state.frontFace  = FrontFace::CounterClockwise;
            state.primitiveType = PrimitiveType::Triangles;
            g_renderBackend->SetPipelineState(state);

            g_renderBackend->BindShader(shader);
            if (g_atlasBuilder) {
                g_renderBackend->BindTexture(g_atlasBuilder->GetBackendTextureHandle(), 0);
            }
            g_renderBackend->SetUniformFloat(shader, "uAlphaTest", 0.01f);
            g_renderBackend->SetUniformVec4(shader, "uPortalClipPlane", ::Render::ChunkRenderer::PortalEntityClipPlane());

            // A falling block IS in the world, so it fades into fog and dims at
            // night exactly like the terrain it fell from. Packing matches
            // ChunkRenderer's so the two fade at the same rate.
            const auto& env = EnvironmentState::Get().Frame();
            // The instanced path carries each entity's light per instance
            // (block_instanced.vert); the per-entity path sets it per draw.
            EntityEnvironment::SetDrawLight(shader, glm::vec3(1.0f));
            g_renderBackend->SetUniformVec4(shader, "uFogColor",
                glm::vec4(env.fogColor, 1.0f));
            g_renderBackend->SetUniformVec4(shader, "uFogEnv",
                glm::vec4(env.fogEnvStart, env.fogEnvEnd, env.fogRdStart, env.fogRdEnd));
            // The instance translations are render-space, so the fog's
            // camera is the render-space eye too.
            g_renderBackend->SetUniformVec3(shader, "uCameraPos", Render::ToRender(cameraPos));

            // The instanced shader multiplies by the per-instance model matrix
            // itself, so it wants the view-projection alone. The per-entity
            // path folds model into uMVP inside its loop instead.
            if (useInstanced) {
                g_renderBackend->SetUniformMat4(shader, "uViewProj", viewProj);
                // It lights each vertex from the instance's cells through
                // MC's lightmap — this view's (the camera's, or a portal far
                // side's own), on texture slot 3 (GL unit 3; Vulkan
                // descriptor set 5), exactly as the terrain shaders read it.
                const TextureHandle lm = Lightmap::Get().TextureFor(env);
                if (lm != INVALID_TEXTURE) {
                    g_renderBackend->BindTexture(lm, 3);
                    g_renderBackend->SetUniformInt(shader, "uLightmap", 3);
                }
            }
        };

        // ── Pass 1: gather ────────────────────────────────────────────────
        //
        // Nothing is built or uploaded here. This used to rebuild the block's
        // mesh and issue TWO GPU uploads PER ENTITY PER FRAME — with a
        // thousand primed TNT that is a thousand identical mesh builds and two
        // thousand uploads every frame, which a Tracy capture put at ~21.6 ms
        // of frame time, 67% of all CPU in the trace.
        //
        // It was also a latent Vulkan correctness bug. MobParticleSystem.cpp
        // documents the rule: the Vulkan backend records buffer copies
        // immediately but executes draws at submit, so uploading between draws
        // clobbers the earlier upload. A thousand identical TNT hid it; a mix
        // of falling sand, anvils and TNT would have drawn every entity with
        // the LAST one's mesh.
        //
        // The per-entity decision — type, shouldRender, distance, frustum,
        // transform — is pure: it reads the entity and the client's block view
        // and writes one DrawItem. So it runs across the worker pool in fixed
        // slices of the dense list, each slice appending to its own partial
        // list, and the partials are concatenated in slice order — the same
        // items in the same order the serial walk produced. A 176k-block sand
        // pyramid spent 11 ms a frame in this loop serially, most of it the
        // shouldRender block lookup.
        // Read from the tick's proxies, not the entities: see
        // ClientMobManager::BlockEntityProxy for why. Nothing below touches
        // a Mob.
        // `sectionGate` — the visible-section test (EntityCulling) reads the
        // chunk map, which asserts main-thread access, so the parallel gather
        // runs with it OFF. That is also where it would cost more than it
        // saves: past a few thousand entities the per-entity lookups outweigh
        // the draws they could skip, and the frustum test still stands.
        // The falling-block light cache (see LightCacheEntry): keyed on the
        // bound level's light version, and dropped whole when the level
        // itself changes (a dimension switch rebinds the block access).
        const Client::ClientBlockAccess* lightLevel = Client::g_clientBlockAccess;
        const uint32_t lightVersion = static_cast<uint32_t>(lightLevel ? lightLevel->LightVersion() : 0);
        if (m_lightCacheLevel != lightLevel) {
            m_lightCache.clear();
            m_lightCacheLevel = lightLevel;
        }

        const auto gatherOne = [&](const Client::BlockEntityProxy& px, size_t index,
                                   std::vector<DrawItem>& out, bool sectionGate) {
            if (!px.drawable) return;
            const bool falling = px.type == static_cast<uint8_t>(Game::EntityTypeId::FallingBlock);

            const Game::BlockState state = Game::BlockState::FromRawId(px.stateRaw);
            if (state.Block() == Game::BlockID::Air) return;

            // MC FallingBlockRenderer.shouldRender:
            //     return entity.getBlockState() != level.getBlockState(entity.blockPosition());
            //
            // Skip the entity whenever the cell it occupies ALREADY holds the
            // block it is carrying. Two moments need this and both are visible
            // without it:
            //
            //   * at spawn, the client has the entity before it has the
            //     block-removal packet, so the block and the entity draw on top
            //     of each other;
            //   * on landing, the entity lingers until its removal packet
            //     arrives, overlapping the block it just became.
            //
            // Two co-located copies of the same model z-fight, and for an
            // asymmetric block like an anvil that reads as the texture flipping
            // rather than as a double image.
            //
            // The lookup is asked only when it can say yes. Both moments put
            // the entity at rest against its own cell: at spawn its feet are
            // at the cell's exact y (InitFall), and on landing it is on the
            // ground (the server clamps it to the block top; the client's
            // MoveApproximate stops it in place with onGround set). An entity
            // that is airborne AND mid-cell is falling through air its own
            // block is not in, so the lookup would answer "draw". The one
            // case this changes: the client already holding the landed block
            // in a cell an airborne entity is still passing through (its
            // block-change packet outran its own simulation); that draws one
            // extra frame of the entity inside the block before its move
            // collides and sets onGround.
            if (falling && blocks) {
                const double y = px.pos.y;
                const bool atRest = px.onGround || std::abs(y - std::round(y)) < 1.0e-3;
                if (atRest) {
                    const glm::ivec3 bp(static_cast<int>(std::floor(px.pos.x)),
                                        static_cast<int>(std::floor(px.pos.y)),
                                        static_cast<int>(std::floor(px.pos.z)));
                    if (blocks->GetBlockState(bp.x, bp.y, bp.z).RawId() == state.RawId()) {
                        return;
                    }
                }
            }

            // Sub-tick interpolation, same as every other entity renderer here:
            // at 0.04 gravity a falling block moves visibly between ticks, and
            // stepping would read as a stutter.
            const glm::dvec3 interp =
                px.prevPos + (px.pos - px.prevPos) * static_cast<double>(partialTick);
            const glm::vec3 worldPos(interp);

            const glm::vec3 toCam = worldPos - cameraPos;
            if (glm::dot(toCam, toCam) > cullSq) return;

            // MC inflates the culling box by 0.5 so an entity straddling a
            // plane is not popped, and substitutes a 2-block box when the real
            // one is degenerate or NaN. Both reproduced here. The box is the
            // entity's own (feet-anchored, half extents from its type), built
            // directly around the INTERPOLATED position — culling on one
            // position and drawing at another pops entities at the screen
            // edge.
            {
                const glm::vec3 size(2.0f * px.half.x, 2.0f * px.half.y, 2.0f * px.half.z);
                const bool degenerate =
                    !(size.x > 0.0f) || !(size.y > 0.0f) || !(size.z > 0.0f);
                glm::vec3 bmin, bmax;
                if (degenerate) {
                    bmin = worldPos - glm::vec3(2.0f);
                    bmax = worldPos + glm::vec3(2.0f);
                } else {
                    bmin = worldPos - glm::vec3(px.half.x + 0.5f, 0.5f, px.half.z + 0.5f);
                    bmax = worldPos + glm::vec3(px.half.x + 0.5f, size.y + 0.5f, px.half.z + 0.5f);
                }
                if (frustum.TestAABB(bmin, bmax) == FrustumResult::Outside) return;
                if (!EntityCulling::PassesCrossingFilter(bmin, bmax)) return;
                // MC isSectionCompiledAndVisible, tightened by the occlusion
                // BFS — see EntityCulling.hpp.
                if (sectionGate && !EntityCulling::BoxTouchesVisibleSection(bmin, bmax)) return;
            }

            // The instance translation is what the GPU sees, so it is the
            // RENDER-space position: the origin subtracted from the double
            // `interp`, never from the float `worldPos` the culls used.
            Instance inst{Render::ToRender(interp), 1.0f};
            bool whiteFlash = false;

            // MC FallingBlockRenderer.extractRenderState: the render state's
            // blockPos is containing(x, boundingBox.maxY, z) at the entity's
            // tick position — where its light and its biome are read.
            const glm::ivec3 lightCell(static_cast<int>(std::floor(px.pos.x)),
                                       static_cast<int>(std::floor(px.pos.y + 2.0 * px.half.y)),
                                       static_cast<int>(std::floor(px.pos.z)));
            if (falling) {
                // Its 3x3x3 light, which block_instanced.vert blends per
                // vertex as MC's ModelBlockRenderer would over the
                // MovingBlockRenderState. Recomputed only when the cell, the
                // carried state or the level's light moved (the slot is this
                // gather's own: slices never share an index).
                LightCacheEntry& cached = m_lightCache[index];
                if (cached.lightVersion != lightVersion || cached.cell != lightCell ||
                    cached.stateRaw != state.RawId()) {
                    FillFallingLight(state, lightCell, cached.cells);
                    cached.cell         = lightCell;
                    cached.stateRaw     = state.RawId();
                    cached.lightVersion = lightVersion;
                }
                std::memcpy(inst.cells, cached.cells, sizeof(cached.cells));
            } else {
                // MC TntRenderer: the entity's packed light
                // (getPackedLightCoords at PrimedTnt's eyeHeight 0.15), one
                // for the whole model — the own cell, which every vertex of
                // the shared mesh reads.
                const int packed = EntityEnvironment::PackedLightAt(interp + glm::dvec3(0.0, 0.15, 0.0));
                inst.cells[kOwnCell] = PackCell(packed);
            }

            if (falling) {
                // MC FallingBlockRenderer: translate(-0.5, 0, -0.5). The entity
                // position is its FEET at the cell's horizontal centre, so this
                // puts the model's own origin corner back where the block was.
                inst.translate += glm::vec3(-0.5f, 0.0f, -0.5f);
            } else {
                // MC TntRenderer: lift half a block, swell, then centre.
                const float fuse = static_cast<float>(px.fuse) - partialTick + 1.0f;
                float scale = 1.0f;
                if (fuse < 10.0f) {
                    // g^4, not g — the quartic is what keeps TNT calm until the
                    // last half second and then lurches.
                    float g = std::clamp(1.0f - fuse / 10.0f, 0.0f, 1.0f);
                    g *= g;
                    g *= g;
                    scale = 1.0f + g * 0.3f;
                }
                // MC writes the chain as
                //     translate(0, 0.5, 0)
                //     scale(s)
                //     mulPose(Axis.YP.rotationDegrees(-90))
                //     translate(-0.5, -0.5,  0.5)
                //     mulPose(Axis.YP.rotationDegrees( 90))
                // The rotate/unrotate PAIR around a translation collapses
                // exactly — R(-90) * T(v) * R(90) == T(R(-90) * v), and R(-90)
                // applied to (-0.5, -0.5, 0.5) is (-0.5, -0.5, -0.5) — so the
                // chain is T(0, 0.5, 0) * S(s) * T(-0.5, -0.5, -0.5), which on a
                // vertex v is s * v + (0, 0.5, 0) - s * 0.5: a scale about the
                // cube's centre, lifted half a block. That is the (translate,
                // scale) pair below. Do not "restore" the two rotations; they
                // cancel.
                inst.translate += glm::vec3(-0.5f * scale, 0.5f - 0.5f * scale, -0.5f * scale);
                inst.scale = scale;
                // MC: white on alternating 5-tick windows, so it blinks about
                // twice a second for the whole fuse.
                whiteFlash = (static_cast<int>(fuse) / 5) % 2 == 0;
            }

            DrawItem item{state, inst, whiteFlash};
            if (falling) {
                item.falling = true;
                // MC FallingBlockRenderer.extractRenderState: the render
                // state's blockPos is containing(x, boundingBox.maxY, z), and
                // its biome is level.getBiome(blockPos) — looked up after the
                // gather (main thread), and only for a state whose tint
                // reads one (sand and gravel never do).
                const BlockTint::Source src = BlockTint::ProfileOf(state.Block()).source;
                if (src == BlockTint::Source::Biome || src == BlockTint::Source::FlowerBed) {
                    item.biomeTint = true;
                    item.tintCell = lightCell;
                }
            }
            out.push_back(item);
        };

        std::vector<DrawItem>& items = m_items;
        items.clear();
        // This call's own meshes (moving blocks, biome-tinted falling
        // blocks); copied into the streaming set in pass 2 below.
        m_customMeshes.clear();
        if (!movingBlocksOnly) {
            PROFILE_ZONE_N("BlockCube.Gather");
            // Two proxy lists: the mob manager's (primed TNT, and any falling
            // block that is a Mob there) and the compact falling-block store's.
            // Walked as one index space so the slices stay even.
            const std::vector<Client::BlockEntityProxy>& listA =
                Client::g_clientMobManager->BlockProxies();
            static const std::vector<Client::BlockEntityProxy> kNone;
            const std::vector<Client::BlockEntityProxy>& listB =
                Client::g_clientFallingBlocks ? Client::g_clientFallingBlocks->Proxies() : kNone;
            const size_t nA = listA.size();
            const size_t n  = nA + listB.size();
            const auto at = [&](size_t i) -> const Client::BlockEntityProxy& {
                return i < nA ? listA[i] : listB[i - nA];
            };
            // One cache slot per proxy index, sized before the workers run.
            if (m_lightCache.size() < n) m_lightCache.resize(n);
            // Everything past the instance cap is silently dropped at submit
            // anyway; stop collecting once no more could be drawn. (The
            // per-entity path has no cap, but it also has no business drawing
            // a quarter of a million entities one call each.)
            const size_t gatherCap = kMaxInstances;

            constexpr size_t kParallelMin = 4096;
            constexpr size_t kSlice       = 2048;
            if (n >= kParallelMin && Core::ParallelWidth() > 1) {
                const size_t slices = (n + kSlice - 1) / kSlice;
                if (m_gatherParts.size() < slices) m_gatherParts.resize(slices);
                Core::ParallelFor(slices, 1, [&](size_t si) {
                    std::vector<DrawItem>& part = m_gatherParts[si];
                    part.clear();
                    const size_t begin = si * kSlice;
                    const size_t end   = std::min(n, begin + kSlice);
                    for (size_t i = begin; i < end; ++i) gatherOne(at(i), i, part, false);
                });
                size_t total = 0;
                for (size_t si = 0; si < slices; ++si) total += m_gatherParts[si].size();
                items.reserve(std::min(total, gatherCap));
                for (size_t si = 0; si < slices && items.size() < gatherCap; ++si) {
                    const std::vector<DrawItem>& part = m_gatherParts[si];
                    const size_t room = gatherCap - items.size();
                    items.insert(items.end(), part.begin(),
                                 part.begin() + static_cast<std::ptrdiff_t>(std::min(room, part.size())));
                }
            } else {
                items.reserve(n);
                for (size_t i = 0; i < n && items.size() < gatherCap; ++i) {
                    gatherOne(at(i), i, items, true);
                }
            }
        }

        PROFILE_PLOT("BlockCube/Entities",
                     static_cast<int64_t>(Client::g_clientMobManager->All().size()));
        // Moving blocks submitted by the block-entity renderers this pass
        // (see SubmitMovingBlock). No culling beyond the frustum: there are
        // at most a handful, and each lives a few ticks.
        if (movingBlocksOnly) {
            std::vector<BlockModelQuad> quads;
            for (const MovingBlock& mb : m_movingBlocks) {
                // MC tesselates a moving block from its block model, and a
                // block its entity draws (chest, sign, banner, skull, …) has
                // a model with no elements — nothing. The cube fallback
                // (BuildBlockCubeMesh) would texture a phantom cube with the
                // whole atlas instead; the carried entity's own renderer
                // (PistonRenderer::RenderCarried) is what shows the block.
                if (Game::BlockRegistry::GetBlockModel(mb.state).elements.empty()) continue;
                const glm::vec3 bmin(mb.worldMin);
                const glm::vec3 bmax = bmin + glm::vec3(1.0f);
                if (frustum.TestAABB(bmin, bmax) == FrustumResult::Outside) continue;
                Instance inst{Render::ToRender(mb.worldMin), 1.0f};
                DrawItem item{mb.state, inst, false};
                // Its own mesh, lit per vertex the way the section mesher
                // lights the block at rest in `lightCell` (MC
                // MovingBlockFeatureRenderer: ModelBlockRenderer over the
                // level's light at the MovingBlockRenderState's blockPos),
                // tinted and face-shaded as the mesher would — the light,
                // AO, tint and shade are baked into the vertex colour, so
                // the instance is flagged pre-lit (kCellFlagPreLit).
                CustomMesh mesh;
                BuildStateMesh(mb.state, mesh.verts, mesh.idx, quads);
                if (!mesh.verts.empty()) {
                    LightMovingBlock(mb.state, mb.lightCell, mb.restingStandIn, mesh.verts, mesh.idx, quads);
                    item.customMesh = static_cast<int>(m_customMeshes.size());
                    item.meshKey    = 0x80000000u | static_cast<uint32_t>(m_customMeshes.size());
                    item.preLit     = true;
                    m_customMeshes.push_back(std::move(mesh));
                } else {
                    item.meshKey = mb.state.RawId();
                    item.instance.cells[kOwnCell] =
                        PackCell(EntityEnvironment::LevelLightCoordsAt(mb.lightCell));
                }
                items.push_back(item);
            }
            m_movingBlocks.clear();
        }
        {
            // Falling blocks: MC draws them through MovingBlockFeatureRenderer
            // like a piston's carried block, so their faces take the level's
            // cardinal lighting, their quads the block's in-world tint and
            // their vertices the render state's AO and the level's light
            // around blockPos (the instance's cells, blended per vertex by
            // the recipes BuildFallingMesh writes).
            // One shared mesh per state (kFallingMeshBit keeps it apart from
            // primed TNT's), and one per (state, biome colour) for the rare
            // biome-tinted falling block (a summoned grass block or leaves).
            std::unordered_map<uint64_t, int> tintedMeshes;
            const Client::ClientBlockAccess* level = Client::g_clientBlockAccess;
            for (DrawItem& item : items) {
                if (!item.falling || item.customMesh >= 0) continue;
                if (!item.biomeTint) {
                    item.meshKey = item.state.RawId() | kFallingMeshBit;
                    continue;
                }
                const BlockTint::Profile& profile = BlockTint::ProfileOf(item.state.Block());
                const BlockTint::Channel channel = profile.source == BlockTint::Source::FlowerBed
                                                       ? BlockTint::Channel::Grass : profile.channel;
                // MC MovingBlockRenderState.getBlockTint: the one biome read
                // at blockPos, resolved there — no blend. No level: -1.
                const glm::ivec3& c = item.tintCell;
                const uint32_t colour = level
                    ? BlockTint::ChannelColor(channel, level->GetBiome(c.x, c.y, c.z), c.x, c.z) & 0xFFFFFFu
                    : BlockTint::kUntinted;
                const uint64_t key = (static_cast<uint64_t>(item.state.RawId()) << 32) | colour;
                const auto [it, inserted] = tintedMeshes.try_emplace(key, static_cast<int>(m_customMeshes.size()));
                if (inserted) {
                    CustomMesh mesh;
                    BuildFallingMesh(item.state, colour, useInstanced, mesh.verts, mesh.idx);
                    m_customMeshes.push_back(std::move(mesh));
                }
                item.customMesh = it->second;
                item.meshKey    = 0x80000000u | static_cast<uint32_t>(it->second);
            }
        }
        for (DrawItem& item : items) {
            if (item.customMesh < 0 && item.meshKey == 0) item.meshKey = item.state.RawId();
            // The light is in the vertex colours already.
            if (item.preLit) item.instance.cells[kCellFlags] |= kCellFlagPreLit;
        }

        PROFILE_PLOT("BlockCube/Draws", static_cast<int64_t>(items.size()));
        if (items.empty()) return;

        // ── Pass 2: build each DISTINCT state once ────────────────────────
        //
        // A thousand TNT are a thousand copies of one mesh. Build per state,
        // append into one combined buffer, and record the index range.
        struct MeshRange { uint32_t firstIndex; uint32_t indexCount; };
        std::unordered_map<uint32_t, MeshRange> ranges;

        verts.clear();
        idx.clear();
        std::vector<ItemCubeVert> stateVerts;
        std::vector<uint32_t>     stateIdx;

        {
            PROFILE_ZONE_N("BlockCube.BuildBatch");
            for (const DrawItem& item : items) {
                const uint32_t key = item.meshKey;
                if (ranges.find(key) != ranges.end()) continue;

                if (item.customMesh >= 0) {
                    stateVerts = m_customMeshes[static_cast<size_t>(item.customMesh)].verts;
                    stateIdx   = m_customMeshes[static_cast<size_t>(item.customMesh)].idx;
                } else if (item.falling) {
                    BuildFallingMesh(item.state, BlockTint::kUntinted, useInstanced, stateVerts, stateIdx);
                } else {
                    BuildStateMesh(item.state, stateVerts, stateIdx);
                }
                if (stateVerts.empty() || stateIdx.empty()) {
                    ranges.emplace(key, MeshRange{0, 0});
                    continue;
                }

                // Bounded by what is left of this frame's streaming set.
                // Dropping a state is better than overrunning it; with one
                // block model per state this is only reachable with an
                // implausible variety on screen at once.
                if (m_vertCursor + verts.size() + stateVerts.size() > kItemCubeMaxVerts ||
                    m_idxCursor  + idx.size()   + stateIdx.size()   > kItemCubeMaxIdx) {
                    ranges.emplace(key, MeshRange{0, 0});
                    continue;
                }

                // Indices are absolute into the set's vertex buffer: this
                // call's vertices start at the frame cursor. The recorded
                // firstIndex is likewise absolute into the index buffer.
                const uint32_t baseVertex = static_cast<uint32_t>(m_vertCursor + verts.size());
                const uint32_t firstIndex = static_cast<uint32_t>(m_idxCursor + idx.size());

                verts.insert(verts.end(), stateVerts.begin(), stateVerts.end());
                // Base vertex folded into the indices rather than passed to the
                // draw: DrawIndexed takes an index offset but no base-vertex, and
                // one combined buffer is what keeps this to a single upload.
                for (uint32_t v : stateIdx) idx.push_back(v + baseVertex);

                ranges.emplace(key, MeshRange{firstIndex,
                                              static_cast<uint32_t>(stateIdx.size())});
            }
        }

        if (idx.empty()) return;

        beginPass();

        // ── Pass 3: ONE upload, then every draw ───────────────────────────
        {
            PROFILE_ZONE_N("BlockCube.Upload");
            g_renderBackend->UpdateBuffer(fb.vb, m_vertCursor * sizeof(ItemCubeVert),
                verts.size() * sizeof(ItemCubeVert), verts.data());
            g_renderBackend->UpdateBuffer(fb.ib, m_idxCursor * sizeof(uint32_t),
                idx.size() * sizeof(uint32_t), idx.data());
            m_vertCursor += verts.size();
            m_idxCursor  += idx.size();
        }

        {
            PROFILE_ZONE_N("BlockCube.Submit");

            // MC TntRenderer -> TntMinecartRenderer.submitWhiteSolidBlock, which
            // sets the OVERLAY texture coordinate rather than tinting. See
            // kTntFlashOverlay for the exact texel it lands on.
            const auto overlayOf = [](bool flash) {
                return flash ? kTntFlashOverlay : glm::vec4(0.0f);
            };

            if (useInstanced) {
                // ── Group, then one draw per group ─────────────────────────
                //
                // A group is (block state, flash state): everything in it
                // shares geometry AND the one uniform that is not per-instance,
                // so it collapses to a single glDrawElementsInstanced. A pure
                // TNT pile is two groups no matter how many entities it holds.
                //
                // Keyed on the raw state id and the flash bit together. Sorting
                // `items` would do the same job, but building the groups
                // directly avoids moving a DrawItem per entity.
                std::unordered_map<uint64_t, std::vector<Instance>> groups;
                groups.reserve(8);
                for (const DrawItem& item : items) {
                    const auto it = ranges.find(item.meshKey);
                    if (it == ranges.end() || it->second.indexCount == 0) continue;
                    const uint64_t key = (static_cast<uint64_t>(item.meshKey) << 1) |
                                         (item.whiteFlash ? 1u : 0u);
                    groups[key].push_back(item.instance);
                }

                // Room for this call past what the frame's earlier calls
                // wrote; a grow moves to a fresh buffer at offset 0. At the
                // cap it can't grow, and the per-group guard below drops
                // what doesn't fit, as before.
                EnsureInstanceCapacity(fb, m_instCursor + items.size());
                // Past what this frame's earlier calls wrote (m_instCursor).
                size_t instanceBytesUsed = m_instCursor * sizeof(Instance);
                for (auto& [key, insts] : groups) {
                    if (insts.empty()) continue;
                    const uint32_t rawState = static_cast<uint32_t>(key >> 1);
                    const bool     flash     = (key & 1u) != 0;
                    const auto it = ranges.find(rawState);
                    if (it == ranges.end() || it->second.indexCount == 0) continue;

                    // Each group at its own offset in the one instance buffer:
                    // on Vulkan the buffer is read at EXECUTION time, so groups
                    // written over each other would all draw the LAST group's
                    // transforms; on GL the backend re-points the instance
                    // attribute at the offset. The gather cap is the buffer
                    // size, so the whole item list fits; the guard is for the
                    // per-entity fallback's sake only.
                    const size_t groupBytes = insts.size() * sizeof(Instance);
                    if (instanceBytesUsed + groupBytes >
                        fb.instCapacity * sizeof(Instance)) {
                        continue;   // buffer full — excess groups skip a frame
                    }
                    g_renderBackend->UpdateBuffer(fb.inst, instanceBytesUsed,
                        groupBytes, insts.data());
                    g_renderBackend->SetUniformVec4(shader, "uOverlayColor",
                                                    overlayOf(flash));
                    g_renderBackend->DrawIndexedInstanced(
                        fb.instMesh, it->second.indexCount, it->second.firstIndex,
                        static_cast<uint32_t>(insts.size()),
                        static_cast<uint32_t>(instanceBytesUsed));
                    instanceBytesUsed += groupBytes;
                }
                m_instCursor = instanceBytesUsed / sizeof(Instance);
                fb.instFrameUsed = std::max(fb.instFrameUsed, m_instCursor);
            } else {
                for (const DrawItem& item : items) {
                    const auto it = ranges.find(item.meshKey);
                    if (it == ranges.end() || it->second.indexCount == 0) continue;

                    // The same transform the instanced shader applies:
                    // translate, then a uniform scale of the model.
                    const glm::mat4 model =
                        glm::scale(glm::translate(glm::mat4(1.0f), item.instance.translate),
                                   glm::vec3(item.instance.scale));
                    g_renderBackend->SetUniformVec4(shader, "uOverlayColor",
                                                    overlayOf(item.whiteFlash));
                    // One light per draw: the own cell's (its meshes were
                    // built without light recipes), white for a pre-lit mesh.
                    {
                        namespace LC = Game::Lighting::LightCoords;
                        const uint8_t own = item.instance.cells[kOwnCell];
                        EntityEnvironment::SetDrawLight(shader, item.preLit
                            ? glm::vec3(1.0f)
                            : EntityEnvironment::LightColor(LC::Pack(own & 15, own >> 4)));
                    }
                    g_renderBackend->SetUniformMat4(shader, "uMVP", viewProj * model);
                    g_renderBackend->SetUniformMat4(shader, "uModel", model);   // fog from the RENDER-space position
                    // block.vert clips in aPos space, which is model space on
                    // this path — give it the plane in model space (Mᵀ p). See
                    // ItemEntityRenderer for the full note. `model` is render-
                    // space (the instance translate is) and the plane is the
                    // render-space one, so the transpose is consistent.
                    {
                        const glm::vec4 plane = ::Render::ChunkRenderer::PortalEntityClipPlane();
                        if (plane.x != 0.0f || plane.y != 0.0f || plane.z != 0.0f) {
                            g_renderBackend->SetUniformVec4(shader, "uPortalClipPlane",
                                                            glm::transpose(model) * plane);
                        }
                    }
                    g_renderBackend->DrawIndexed(fb.mesh, it->second.indexCount,
                                                 it->second.firstIndex);
                }
            }
        }

        if (anyDrawn) {
            // Clear the overlay before leaving. On GL the uniform is per
            // program and this is belt-and-braces, but the Vulkan Common UBO is
            // GLOBAL — a flash left set here would follow into whatever draws
            // next, and the terrain would strobe white in time with the TNT.
            g_renderBackend->SetUniformVec4(shader, "uOverlayColor", glm::vec4(0.0f));
            g_renderBackend->UnbindMesh();
        }
    }

    void BlockCubeEntityRenderer::SubmitMovingBlock(Game::BlockState state, const glm::dvec3& worldMin,
                                                    const glm::ivec3& lightCell, bool restingStandIn) {
        if (m_movingBlocks.size() < 4096) {
            m_movingBlocks.push_back(MovingBlock{state, worldMin, lightCell, restingStandIn});
        }
    }

    // The section mesher's per-vertex colour (Mesher::AddBlockFace: the
    // tint through BlockTint, the face shade through Game::ElementShade, AO
    // and light through the shared BlockModelLighter) applied to a
    // block-model mesh in its unit cell, sampling the live client level
    // around `cell`: the smooth path's four-corner AO and light blend when
    // the model and the Smooth Lighting option ask for it, else one flat
    // light per face. Each vertex's colour is tint × shade × AO × the
    // lightmap colour of its light coords — what the section mesh and
    // terrain.vert make of the same numbers — so a block drawn here and the
    // same block in a section mesh are the same colour, and a piston's
    // base, head or carried block shows no step when it is handed between
    // the two.
    //
    // With the builder's per-quad record each quad uses exactly what the
    // mesher uses: the face it was authored as, its cullface (flat light),
    // its unrotated corners for AO and its rotated ones for light. Without
    // it (the cube fallback) the face is read off the triangle's normal,
    // the flat path takes the face's own side when the quad lies on it (MC's
    // faceCubic rule, the cullface every vanilla model names), and the
    // builder's colour is kept.
    void BlockCubeEntityRenderer::LightMovingBlock(Game::BlockState state, const glm::ivec3& cell,
                                                   bool restingStandIn,
                                                   std::vector<ItemCubeVert>& verts,
                                                   const std::vector<uint32_t>& idx,
                                                   const std::vector<BlockModelQuad>& quads) {
        if (!Client::g_clientBlockAccess) return;
        const Client::ClientBlockAccess& blocks = *Client::g_clientBlockAccess;
        const ClientLevelLighter level{blocks};
        const Mesher::MeshOptions options = Mesher::GetMeshOptions();
        const bool smooth = BlockModelLighter::UsesSmoothLighting(
            state, Game::BlockRegistry::GetBlockModel(state).ambientOcclusion, options.smoothLighting);

        const bool haveQuads = !quads.empty() &&
                               quads.size() * 4 == verts.size() && quads.size() * 6 == idx.size();
        if (haveQuads) {
            // MC BlockTintSource.colorInWorld against the level the block is
            // drawn with. In flight: MovingBlockRenderState.getBlockTint —
            // the state's one biome, level.getBiome(blockPos), resolved at
            // blockPos, no blend. Standing in for the section mesh: the
            // mesher's ClientLevel.calculateBlockTint blend at the cell.
            uint32_t channelColor[4];
            bool     channelKnown[4] = {false, false, false, false};
            const auto biomeColor = [&](BlockTint::Channel channel) -> uint32_t {
                const size_t ci = static_cast<size_t>(channel);
                if (!channelKnown[ci]) {
                    channelKnown[ci] = true;
                    channelColor[ci] = restingStandIn
                        ? BlockTint::BlendedColor(channel, cell.x, cell.y, cell.z, options.biomeBlendRadius,
                                                  [&](int x, int y, int z) { return blocks.GetBiome(x, y, z); })
                        : BlockTint::ChannelColor(channel, blocks.GetBiome(cell.x, cell.y, cell.z), cell.x, cell.z);
                }
                return channelColor[ci];
            };
            std::vector<glm::vec3> base;
            QuadBaseColors(state, quads, BoundLevelNetherCardinalLight(), biomeColor, base);

            for (size_t qi = 0; qi < quads.size(); ++qi) {
                const BlockModelQuad& q = quads[qi];
                const Game::Direction dir = ToDirection(q.dir);
                const size_t first = qi * 4;
                // The baked quad as MC's lighter sees it: rotated (light);
                // the unrotated corners are what the mesher samples AO on.
                glm::vec3 rotated[4];
                glm::vec3 unrotated[4];
                for (int v = 0; v < 4; ++v) {
                    const ItemCubeVert& vert = verts[first + static_cast<size_t>(v)];
                    rotated[v] = glm::vec3(vert.x, vert.y, vert.z);
                    unrotated[v] = q.unrotated[v];
                }

                float ao[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                int coords[4];
                if (smooth) {
                    BlockModelLighter::QuadShade(level, state, cell.x, cell.y, cell.z, dir, unrotated, ao);
                    BlockModelLighter::QuadLightSmooth(level, state, cell.x, cell.y, cell.z, dir, rotated, coords);
                } else {
                    Game::Direction cullface{};
                    const bool culled = q.cullfaceDir >= 0;
                    if (culled) cullface = ToDirection(static_cast<Game::FaceDir>(q.cullfaceDir));
                    const int flat = BlockModelLighter::QuadLightFlat(level, state, cell.x, cell.y, cell.z, dir,
                                                                     culled ? &cullface : nullptr, rotated);
                    coords[0] = coords[1] = coords[2] = coords[3] = flat;
                }

                for (int v = 0; v < 4; ++v) {
                    ItemCubeVert& vert = verts[first + static_cast<size_t>(v)];
                    const glm::vec3 c = base[qi] * ao[v] * EntityEnvironment::LightColorCoords(coords[v]);
                    vert.r = ToByte(c.r);
                    vert.g = ToByte(c.g);
                    vert.b = ToByte(c.b);
                }
            }
            return;
        }

        // Every vertex is lit once, by the first quad that owns it (the
        // model builder gives each face its own four).
        std::vector<uint8_t> lit(verts.size(), 0);
        // Every six indices are one quad (two triangles over four vertices).
        for (size_t i = 0; i + 5 < idx.size(); i += 6) {
            uint32_t quad[4];
            int count = 0;
            for (size_t k = i; k < i + 6 && count < 4; ++k) {
                bool seen = false;
                for (int q = 0; q < count; ++q) if (quad[q] == idx[k]) { seen = true; break; }
                if (!seen) quad[count++] = idx[k];
            }
            if (count < 3) continue;
            for (int q = count; q < 4; ++q) quad[q] = quad[count - 1];

            const ItemCubeVert& a = verts[idx[i]];
            const ItemCubeVert& b = verts[idx[i + 1]];
            const ItemCubeVert& c = verts[idx[i + 2]];
            const glm::vec3 normal = glm::cross(glm::vec3(b.x - a.x, b.y - a.y, b.z - a.z),
                                                glm::vec3(c.x - a.x, c.y - a.y, c.z - a.z));
            const glm::vec3 an = glm::abs(normal);
            const int nAxis = (an.x >= an.y && an.x >= an.z) ? 0 : (an.y >= an.z ? 1 : 2);
            if (an[nAxis] < 1e-6f) continue;
            const bool positive = normal[nAxis] > 0.0f;
            const Game::Direction dir =
                nAxis == 0 ? (positive ? Game::Direction::East  : Game::Direction::West)
              : nAxis == 1 ? (positive ? Game::Direction::Up    : Game::Direction::Down)
                           : (positive ? Game::Direction::South : Game::Direction::North);

            glm::vec3 localPos[4];
            for (int q = 0; q < 4; ++q) {
                const ItemCubeVert& v = verts[quad[q]];
                localPos[q] = glm::vec3(v.x, v.y, v.z);
            }

            float ao[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            int coords[4];
            if (smooth) {
                BlockModelLighter::QuadShade(level, state, cell.x, cell.y, cell.z, dir, localPos, ao);
                BlockModelLighter::QuadLightSmooth(level, state, cell.x, cell.y, cell.z, dir, localPos, coords);
            } else {
                const int flat = BlockModelLighter::QuadLightFlat(level, state, cell.x, cell.y, cell.z, dir,
                                                                 nullptr, localPos);
                coords[0] = coords[1] = coords[2] = coords[3] = flat;
            }

            for (int q = 0; q < count; ++q) {
                if (lit[quad[q]]) continue;
                lit[quad[q]] = 1;
                ItemCubeVert& v = verts[quad[q]];
                const glm::vec3 light = EntityEnvironment::LightColorCoords(coords[q]) * ao[q];
                v.r = static_cast<uint8_t>(std::clamp(static_cast<float>(v.r) * light.r + 0.5f, 0.0f, 255.0f));
                v.g = static_cast<uint8_t>(std::clamp(static_cast<float>(v.g) * light.g + 0.5f, 0.0f, 255.0f));
                v.b = static_cast<uint8_t>(std::clamp(static_cast<float>(v.b) * light.b + 0.5f, 0.0f, 255.0f));
            }
        }
    }

    void BlockCubeEntityRenderer::FillFallingLight(Game::BlockState state, const glm::ivec3& cell,
                                                   uint8_t (&out)[27]) {
        // MC LightCoordsUtil.getLightCoords(state, level, pos) through the
        // MovingBlockRenderState: the level's light engine for the light,
        // the CARRIED state for emissiveRendering and the self-emission
        // floor. The smooth path asks with AIR, but it only runs for a state
        // that neither emits nor renders emissive, where the two agree — so
        // one table serves both of block_instanced.vert's paths.
        namespace L = Game::Lighting;
        if (L::BlockLightProperties::EmissiveRendering(state)) {
            std::memset(out, 0xFF, sizeof(out));   // FULL_BRIGHT: block 15, sky 15
            return;
        }
        if (const Client::ClientBlockAccess* level = Client::g_clientBlockAccess) {
            level->GetLightCube(cell.x, cell.y, cell.z, out);
        } else {
            std::memset(out, 0xF0, sizeof(out));   // no level: open sky, no block light
        }
        const int emission = L::BlockLightProperties::Emission(state);
        if (emission > 0) {
            for (uint8_t& c : out) {
                if ((c & 15) < emission) c = static_cast<uint8_t>((c & 0xF0) | emission);
            }
        }
    }

    void BlockCubeEntityRenderer::BuildFallingMesh(Game::BlockState state, uint32_t biomeColor,
                                                   bool lightRecipes,
                                                   std::vector<ItemCubeVert>& verts,
                                                   std::vector<uint32_t>& idx) {
        std::vector<BlockModelQuad> quads;
        BuildStateMesh(state, verts, idx, quads);
        // The cube fallback keeps its opaque alpha: recipe "own cell", flat.
        if (quads.empty() || quads.size() * 4 != verts.size()) return;
        std::vector<glm::vec3> base;
        QuadBaseColors(state, quads, BoundLevelNetherCardinalLight(),
                       [biomeColor](BlockTint::Channel) { return biomeColor; }, base);

        // MC ModelBlockRenderer.tesselateBlock: tesselateAmbientOcclusion
        // when the option, the model and the state's emission allow it,
        // else tesselateFlat — the same choice the section mesher makes.
        const bool smooth = BlockModelLighter::UsesSmoothLighting(
            state, Game::BlockRegistry::GetBlockModel(state).ambientOcclusion,
            Mesher::GetMeshOptions().smoothLighting);
        const MovingBlockView view{state};

        for (size_t qi = 0; qi < quads.size(); ++qi) {
            const BlockModelQuad& q = quads[qi];
            const Game::Direction dir = ToDirection(q.dir);
            const size_t first = qi * 4;
            // As LightMovingBlock: the baked (rotated) corners place the
            // light, the unrotated ones the AO.
            glm::vec3 rotated[4];
            glm::vec3 unrotated[4];
            for (int v = 0; v < 4; ++v) {
                const ItemCubeVert& vert = verts[first + static_cast<size_t>(v)];
                rotated[v] = glm::vec3(vert.x, vert.y, vert.z);
                unrotated[v] = q.unrotated[v];
            }

            float ao[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            uint8_t recipe;
            if (smooth) {
                BlockModelLighter::QuadShade(view, state, 0, 0, 0, dir, unrotated, ao);
                recipe = static_cast<uint8_t>(
                    kRecipeSmooth | static_cast<uint8_t>(dir) |
                    (BlockModelLighter::FaceCubic(state, dir, rotated) ? kRecipeCubicBit : 0));
            } else if (q.cullfaceDir >= 0) {
                // tesselateFlat: a culled quad reads the cell its cullface names.
                recipe = static_cast<uint8_t>(ToDirection(static_cast<Game::FaceDir>(q.cullfaceDir)));
            } else {
                // An unculled one: the neighbour it lies against, else its own cell.
                recipe = BlockModelLighter::FaceCubic(state, dir, rotated)
                             ? static_cast<uint8_t>(dir) : kRecipeOwnCell;
            }

            for (size_t v = first; v < first + 4; ++v) {
                const glm::vec3 c = base[qi] * ao[v - first];
                verts[v].r = ToByte(c.r);
                verts[v].g = ToByte(c.g);
                verts[v].b = ToByte(c.b);
                if (lightRecipes) verts[v].a = recipe;
            }
        }
    }

    void BlockCubeEntityRenderer::BuildStateMesh(Game::BlockState state,
                                                 std::vector<ItemCubeVert>& verts,
                                                 std::vector<uint32_t>& idx,
                                                 std::vector<BlockModelQuad>& quads) {
        verts.clear();
        idx.clear();
        quads.clear();
        const Game::BlockModel& blockModel = Game::BlockRegistry::GetBlockModel(state);
        if (!BuildBlockModelMeshFrom(blockModel, verts, idx, &quads)) {
            quads.clear();
            BuildBlockCubeMesh(state.Block(), verts, idx);
        }
    }

    void BlockCubeEntityRenderer::BuildStateMesh(Game::BlockState state,
                                                 std::vector<ItemCubeVert>& verts,
                                                 std::vector<uint32_t>& idx) {
        verts.clear();
        idx.clear();

        // The STATE's model, resolved through the SAME call the chunk mesher
        // makes. An anvil's facing and a dripstone's thickness live in the
        // blockstate, so the default-state model would be visibly rotated wrong
        // — and going through the name-based item lookup instead would prefer a
        // `<name>_inventory` model where one exists, which is right for a
        // dropped item and wrong for a falling block.
        //
        // Sharing the mesher's resolution is what guarantees a falling anvil
        // and the anvil it lands as are the same geometry.
        const Game::BlockModel& blockModel = Game::BlockRegistry::GetBlockModel(state);
        if (!BuildBlockModelMeshFrom(blockModel, verts, idx)) {
            BuildBlockCubeMesh(state.Block(), verts, idx);
        }
    }

} // namespace Render
