// File: src/client/renderer/entity/ItemEntityRenderer.cpp
#include "ItemEntityRenderer.hpp"
#include "../core/Frustum.hpp"

#include "../backend/RenderBackend.hpp"
#include "../environment/EnvironmentState.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "../viewmodel/HeldItemSpriteMesh.hpp"
#include "../viewmodel/ItemMeshBuilder.hpp"
#include "client/entity/ItemEntityManager.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/core/Log.hpp"

#ifdef HAS_VULKAN
#include "../backend/vulkan/VKBackend.hpp"
#endif

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <vector>


namespace {

    // ── Per-block-item mesh cache ──────────────────────────────────────────
    //
    // The block-item path used to rebuild the model mesh and issue TWO GPU
    // uploads PER ITEM ENTITY PER FRAME. A crater's worth of dropped cobble is
    // hundreds of identical meshes rebuilt every frame; a Tracy capture put the
    // whole entity-render block at 67% of CPU.
    //
    // It was also a latent Vulkan bug, for the reason MobParticleSystem.cpp
    // spells out: the Vulkan backend records buffer copies immediately but runs
    // draws at submit, so uploading between draws clobbers the earlier upload.
    // A pile of identical cobblestone hid it; a mixed pile would have drawn
    // every stack with the last one's mesh.
    //
    // Sprite items already worked this way (HeldItemSpriteMesh::GetOrBuild);
    // this is the same pattern for blocks. Each entry owns its buffers and is
    // uploaded exactly once, at build.
    struct BlockItemMesh {
        Render::MeshHandle   mesh        = Render::INVALID_MESH;
        Render::BufferHandle vertexBuffer = Render::INVALID_BUFFER;
        Render::BufferHandle indexBuffer  = Render::INVALID_BUFFER;
        uint32_t             indexCount  = 0;
        // Derived from the geometry, so cached with it rather than recomputed
        // by a min/max walk over every vertex every frame.
        float                modelMinY   = 0.0f;
        float                modelDepth  = 1.0f;
        bool                 valid       = false;
    };

    std::unordered_map<std::string, BlockItemMesh> g_blockItemMeshes;

    const BlockItemMesh* GetOrBuildBlockItemMesh(Game::BlockID blockId,
                                                 const std::string& modelOverride) {
        std::string key = std::to_string(static_cast<uint32_t>(blockId));
        key += '|';
        key += modelOverride;

        const auto it = g_blockItemMeshes.find(key);
        if (it != g_blockItemMeshes.end()) {
            return it->second.valid ? &it->second : nullptr;
        }

        BlockItemMesh entry;
        std::vector<Render::ItemCubeVert> verts;
        std::vector<uint32_t>             idx;

        if (!Render::BuildBlockModelMesh(blockId, modelOverride, verts, idx)) {
            Render::BuildBlockCubeMesh(blockId, verts, idx);
        }

        if (!verts.empty() && !idx.empty() && Render::g_renderBackend) {
            float minY = verts[0].y, minZ = verts[0].z, maxZ = verts[0].z;
            for (const auto& v : verts) {
                minY = std::min(minY, v.y);
                minZ = std::min(minZ, v.z);
                maxZ = std::max(maxZ, v.z);
            }
            entry.modelMinY  = minY;
            entry.modelDepth = maxZ - minZ;

            entry.vertexBuffer = Render::g_renderBackend->CreateBuffer(
                Render::BufferUsage::Vertex, verts.size() * sizeof(Render::ItemCubeVert),
                verts.data(), Render::BufferAccess::Static);
            entry.indexBuffer = Render::g_renderBackend->CreateBuffer(
                Render::BufferUsage::Index, idx.size() * sizeof(uint32_t),
                idx.data(), Render::BufferAccess::Static);
            entry.mesh = Render::g_renderBackend->CreateMesh(
                entry.vertexBuffer, entry.indexBuffer, Render::GetBlockVertexLayout());
            entry.indexCount = static_cast<uint32_t>(idx.size());
            entry.valid = true;
        }

        auto [pos, ok] = g_blockItemMeshes.emplace(std::move(key), entry);
        return pos->second.valid ? &pos->second : nullptr;
    }

    void ClearBlockItemMeshCache() {
        if (Render::g_renderBackend) {
            for (auto& [key, e] : g_blockItemMeshes) {
                if (e.mesh != Render::INVALID_MESH) Render::g_renderBackend->DestroyMesh(e.mesh);
                if (e.vertexBuffer != Render::INVALID_BUFFER) Render::g_renderBackend->DestroyBuffer(e.vertexBuffer);
                if (e.indexBuffer != Render::INVALID_BUFFER) Render::g_renderBackend->DestroyBuffer(e.indexBuffer);
            }
        }
        g_blockItemMeshes.clear();
    }

} // namespace

namespace Render {

    namespace {
        constexpr float kPi = 3.14159265358979323846f;

        // MC ItemClusterRenderState.getRenderedAmount — how many copies of the
        // model to stack up so a big pile reads as bigger than a single item.
        int RenderedAmount(int stackCount) {
            if (stackCount <= 1)  return 1;
            if (stackCount <= 16) return 2;
            if (stackCount <= 32) return 3;
            return stackCount <= 48 ? 4 : 5;
        }

        // MC getSeedForItemStack. Seeding per ITEM TYPE (not per entity) is
        // deliberate: the copy jitter then stays identical frame to frame and
        // between clients, instead of shimmering as the RNG is re-walked.
        uint32_t SeedForStack(const Game::ItemStack& stack) {
            return stack.IsEmpty() ? 187u : static_cast<uint32_t>(stack.itemId);
        }

        // Tiny deterministic PRNG for the copy offsets. It does not need to
        // match java.util.Random — the offsets are pure decoration, and unlike
        // the loot roll nothing observable depends on the exact sequence.
        struct Jitter {
            uint32_t s;
            explicit Jitter(uint32_t seed) : s(seed ? seed : 1u) {}
            float Next() {
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                return static_cast<float>(s & 0xFFFFFF) / static_cast<float>(0x1000000);
            }
            // MC's `(random.nextFloat() * 2 - 1) * scale`.
            float Signed(float scale) { return (Next() * 2.0f - 1.0f) * scale; }
        };

        // MC ItemEntityRenderer.ITEM_BUNDLE_OFFSET_SCALE.
        constexpr float kCopyJitter = 0.15f;
    } // namespace

    bool ItemEntityRenderer::Initialize() {
        if (!g_renderBackend) return false;

        // Same block shader the held-item renderer uses: single 2D texture,
        // vertex-colour multiply, alpha test. On Vulkan it must be created
        // through the UBO-aware (portal) layout because the block shaders
        // declare the Common UBO — the plain path bakes a texture-only layout
        // and pipeline creation fails.
        if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
            auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
            m_shader = vk->CreateShaderFromFilesPortal(
                "shaders/block.vert", "shaders/block.frag");
#endif
        } else {
            m_shader = g_renderBackend->CreateShaderFromFiles(
                "shaders/block.vert", "shaders/block.frag");
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[ItemEntityRenderer] failed to load block shader — "
                         "dropped items will not render");
            return false;
        }

        unsigned char white[4] = { 255, 255, 255, 255 };
        m_dummyTexture = g_renderBackend->CreateTexture2D(
            1, 1, TextureFormat::RGBA8, white);

        m_cubeVB = g_renderBackend->CreateBuffer(
            BufferUsage::Vertex, kItemCubeMaxVerts * sizeof(ItemCubeVert),
            nullptr, BufferAccess::Streaming);
        m_cubeIB = g_renderBackend->CreateBuffer(
            BufferUsage::Index, kItemCubeMaxIdx * sizeof(uint32_t),
            nullptr, BufferAccess::Streaming);
        m_cubeMesh = g_renderBackend->CreateMesh(
            m_cubeVB, m_cubeIB, GetBlockVertexLayout());

        m_initialized = true;
        Log::Info("[ItemEntityRenderer] initialized");
        return true;
    }

    void ItemEntityRenderer::Shutdown() {
        if (!g_renderBackend) return;
        ClearBlockItemMeshCache();
        if (m_cubeMesh != INVALID_MESH)  { g_renderBackend->DestroyMesh(m_cubeMesh); m_cubeMesh = INVALID_MESH; }
        if (m_cubeVB   != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(m_cubeVB); m_cubeVB = INVALID_BUFFER; }
        if (m_cubeIB   != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(m_cubeIB); m_cubeIB = INVALID_BUFFER; }
        if (m_dummyTexture != INVALID_TEXTURE) {
            g_renderBackend->DestroyTexture(m_dummyTexture);
            m_dummyTexture = INVALID_TEXTURE;
        }
        if (m_shader != INVALID_SHADER) {
            g_renderBackend->DestroyShader(m_shader);
            m_shader = INVALID_SHADER;
        }
        m_initialized = false;
    }
    void ItemEntityRenderer::SetRenderDistanceChunks(int chunks) {
        // A quarter of the chunks, times 16 blocks per chunk — i.e. chunks * 4.
        // Float rather than integer division so a view distance that is not a
        // multiple of four lands between the steps instead of snapping down a
        // whole chunk.
        m_maxRenderDistance =
            std::max(kMinRenderDistance, static_cast<float>(chunks) * 4.0f);
    }


    void ItemEntityRenderer::Render(const glm::mat4& projection, const glm::mat4& view,
                                    const glm::vec3& cameraPos, float partialTick) {
        if (!m_initialized || !g_renderBackend) return;
        if (!Client::g_itemEntityManager) return;

        const auto& entities = Client::g_itemEntityManager->GetEntities();
        const auto& pickups  = Client::g_itemEntityManager->GetPickups();
        if (entities.empty() && pickups.empty()) return;

        const glm::mat4 viewProj = projection * view;
        const float maxDistSq = m_maxRenderDistance * m_maxRenderDistance;

        // MC EntityRenderer.shouldRender — distance AND frustum. A crater's
        // worth of drops behind the camera used to cost a full draw each.
        const Frustum frustum = Frustum::FromMatrix(viewProj);

        std::vector<ItemCubeVert> verts;
        std::vector<uint32_t>     idx;
        verts.reserve(kItemCubeMaxVerts);
        idx.reserve(kItemCubeMaxIdx);

        // ── Items lying in the world ───────────────────────────────────────
        // Per-frame draw budget: every item is its own draw (worse on
        // Vulkan, which has no instanced path here), and a mass detonation's
        // drop field was measured at a quarter of a frozen frame. Beyond the
        // budget the remaining items simply skip a frame's render — they are
        // still there, still ticking, and the nearest ones win via the
        // distance cull above them.
        constexpr int kMaxItemDrawsPerFrame = 4096;
        int itemDraws = 0;

        for (const auto& [id, ce] : entities) {
            if (itemDraws >= kMaxItemDrawsPerFrame) break;
            const Game::ItemEntity& e = ce.sim;
            if (e.stack.IsEmpty()) continue;

            // Sub-tick blend of the previous and current tick positions. Doing
            // this per frame is what makes a falling item look continuous
            // rather than stepping 20 times a second.
            const glm::vec3 pos = glm::vec3(
                glm::mix(ce.renderPrevPosition, e.pos, static_cast<double>(partialTick)));

            const glm::vec3 d = pos - cameraPos;
            if (glm::dot(d, d) > maxDistSq) continue;

            // MC inflates the culling box by 0.5. A dropped item is 0.25 on a
            // side and bobs vertically, so the inflate is doing real work here
            // rather than being a formality — a tight box would pop items at
            // the screen edge as they rose.
            {
                const glm::vec3 half(Game::ItemEntity::kWidth * 0.5f + 0.5f,
                                     Game::ItemEntity::kHeight     + 0.5f,
                                     Game::ItemEntity::kWidth * 0.5f + 0.5f);
                if (frustum.TestAABB(pos - half, pos + half) == FrustumResult::Outside) {
                    continue;
                }
            }

            // MC's age is in ticks and includes the partial tick, so the bob
            // and spin advance smoothly within a tick rather than in steps.
            DrawItem(e.stack, pos, ce.ageTicks + partialTick, e.bobOffs,
                     viewProj, cameraPos, verts, idx);
            ++itemDraws;
        }

        // ── Items flying into whoever collected them ───────────────────────
        for (const auto& p : pickups) {
            // MC ItemPickupParticleGroup.ParticleInstance.fromParticle:
            //   time = ((life + partialTick) / LIFE_TIME)²
            // The square is an ease-IN — the item creeps away from where it lay
            // and then snaps into the player, which is what gives the pickup
            // its characteristic suck rather than a linear slide.
            float t = (static_cast<float>(p.life) + partialTick)
                    / static_cast<float>(Client::ItemEntityManager::kPickupLifeTicks);
            t = glm::clamp(t, 0.0f, 1.0f);
            t *= t;

            // The collector keeps moving during the flight, so the target is
            // itself interpolated across the tick.
            //
            // Until the first Tick resolves the collector, aim at the item's
            // own resting place — the target slots are still zero, and lerping
            // toward those would sling the item at the world origin for the
            // handful of frames before the tick lands.
            const glm::dvec3 target = p.targetSeeded
                ? glm::mix(p.targetPosOld, p.targetPos, static_cast<double>(partialTick))
                : p.startPos;
            const glm::vec3 pos =
                glm::vec3(glm::mix(p.startPos, target, static_cast<double>(t)));

            const glm::vec3 d = pos - cameraPos;
            if (glm::dot(d, d) > maxDistSq) continue;

            // Frozen age: a collected item keeps the orientation it had when it
            // was picked up instead of continuing to spin as it flies.
            DrawItem(p.stack, pos, p.ageTicks, p.bobOffs,
                     viewProj, cameraPos, verts, idx);
        }
    }

    void ItemEntityRenderer::DrawItem(const Game::ItemStack& stack,
                                      const glm::vec3& pos, float ageTicks,
                                      float bobOffs,
                                      const glm::mat4& viewProj,
                                      const glm::vec3& cameraPos,
                                      std::vector<ItemCubeVert>& verts,
                                      std::vector<uint32_t>& idx) {
            const Game::Item& item = Game::ItemRegistry::Get(stack.itemId);

            // Bob: sin(age/10 + phase) * 0.1 + 0.1, so it oscillates in
            // [0, 0.2] and never dips below the ground.
            const float bob = std::sin(ageTicks / 10.0f + bobOffs) * 0.1f + 0.1f;
            // Spin: age/20 + phase, in RADIANS about Y.
            const float spin = ageTicks / 20.0f + bobOffs;

            const int copies = RenderedAmount(stack.count);
            Jitter jitter(SeedForStack(stack));

            const bool isBlock = (item.renderType == Game::ItemRenderType::Block);

            // ── Build (or fetch) this item's geometry ──────────────────────
            MeshHandle    mesh       = INVALID_MESH;
            uint32_t      indexCount = 0;
            TextureHandle tex        = m_dummyTexture;
            // Scale and lift from the item model's `display.ground` transform:
            // block items are 0.25 with a +3/16 rise, sprite items 0.5 with
            // +2/16 (assets/models/block/block.json and
            // assets/models/item/generated.json respectively). Read from the
            // repo's own asset values rather than assumed.
            float groundScale = 0.25f;
            float groundLift  = 3.0f / 16.0f;

            // Model extent in unit-cube space, used for the hover height and to
            // decide whether the copies fan (flat) or scatter (3D).
            float modelMinY  = 0.0f;
            float modelDepth = 1.0f;
            // Sprite meshes are authored in 0..16 space; everything downstream
            // assumes the unit cube, so they get an extra 1/16 pre-scale.
            float preScale = 1.0f;

            if (isBlock) {
                // Built and uploaded ONCE, then reused — see the cache above.
                // This used to rebuild the mesh and upload twice for every item
                // entity, every frame.
                const auto* entry =
                    GetOrBuildBlockItemMesh(item.blockId, item.blockModelOverride);
                if (!entry) return;   // no geometry to draw

                modelMinY  = entry->modelMinY;
                modelDepth = entry->modelDepth;

                mesh       = entry->mesh;
                indexCount = entry->indexCount;
                if (g_atlasBuilder) tex = g_atlasBuilder->GetBackendTextureHandle();
            } else {
                // Sprite items reuse the cached extruded mesh the hand uses.
                // Layer-0 tint — see HeldItemRenderer. Plant sprites are
                // greyscale and would otherwise lie on the ground grey.
                const uint32_t spriteTint =
                    item.layerTints.empty() ? 0u : item.layerTints[0];
                const auto* entry =
                    HeldItemSpriteMesh::GetOrBuild(item.spriteName, spriteTint);
                if (!entry || entry->indexCount == 0) return;  // sprite failed to load
                mesh        = entry->mesh;
                indexCount  = entry->indexCount;
                tex         = entry->texture;
                groundScale = 0.5f;
                groundLift  = 2.0f / 16.0f;
                preScale    = 1.0f / 16.0f;
                // The extruded sprite spans its full cell vertically and is
                // ~1px thick, which puts it under MC's flat threshold — so it
                // fans along Z rather than scattering in 3D.
                modelMinY   = 0.0f;
                modelDepth  = 1.0f / 16.0f;
            }

            // ── Pipeline state ─────────────────────────────────────────────
            PipelineState state;
            state.depthTestEnabled  = true;
            state.depthWriteEnabled = true;
            state.blendEnabled      = true;
            state.srcBlendFactor    = BlendFactor::SrcAlpha;
            state.dstBlendFactor    = BlendFactor::OneMinusSrcAlpha;
            // Block meshes are closed, so back-face culling is free. The
            // sprite mesh's "outward" side depends on the spin angle, so
            // culling it would make items vanish for half of every rotation.
            state.cullMode  = isBlock ? CullMode::Back : CullMode::None;
            state.frontFace = FrontFace::CounterClockwise;
            state.primitiveType = PrimitiveType::Triangles;
            g_renderBackend->SetPipelineState(state);

            g_renderBackend->BindShader(m_shader);
            g_renderBackend->BindTexture(tex, 0);
            g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest",
                isBlock ? 0.01f : 0.5f);
            g_renderBackend->SetUniformVec4(m_shader, "uPortalClipPlane",
                glm::vec4(0.0f));

            // Unlike the viewmodel, a dropped item IS in the world, so it gets
            // the real fog and sky-brightness environment — an item lying in
            // the distance should fade into the fog like the terrain it sits
            // on, and dim at night.
            // Packing matches ChunkRenderer's — the block shader reads the
            // same four fog fields, so terrain and the items lying on it fade
            // together instead of at different rates.
            const auto& env = EnvironmentState::Get().Frame();
            g_renderBackend->SetUniformFloat(m_shader, "uSkyBrightness", env.skyBrightness);
            g_renderBackend->SetUniformVec4(m_shader, "uFogColor",
                glm::vec4(env.fogColor, 1.0f));
            g_renderBackend->SetUniformVec4(m_shader, "uFogEnv",
                glm::vec4(env.fogEnvStart, env.fogEnvEnd, env.fogRdStart, env.fogRdEnd));
            g_renderBackend->SetUniformVec3(m_shader, "uCameraPos", cameraPos);

            // MC ItemEntityRenderer: lift so the model's lowest point sits
            // ITEM_MIN_HOVER_HEIGHT (1/16) above the entity origin, measured
            // AFTER the ground transform. For a full cube this works out to
            // zero, which is why a dropped block rests 1/16 off the floor.
            const float transformedMinY = groundLift + groundScale * (modelMinY - 0.5f);
            const float minOffsetY = -transformedMinY + 0.0625f;

            // MC FLAT_ITEM_DEPTH_THRESHOLD: thin models fan along their own Z
            // instead of scattering, so a stack of swords reads as a fanned
            // pile rather than an intersecting jumble.
            const float scaledDepth = modelDepth * groundScale * preScale;
            const bool  isFlat      = scaledDepth <= 0.0625f;
            const float fanZ        = scaledDepth * 1.5f;

            for (int copy = 0; copy < copies; ++copy) {
                glm::mat4 model(1.0f);
                model = glm::translate(model, pos + glm::vec3(0.0f, bob + minOffsetY, 0.0f));
                model = glm::rotate(model, spin, glm::vec3(0.0f, 1.0f, 0.0f));

                // Copies after the first are offset so a stack reads as a heap
                // rather than perfectly coincident geometry (which would also
                // z-fight).
                if (isFlat) {
                    // Centre the fan, then step one depth along Z per copy.
                    model = glm::translate(model, glm::vec3(
                        0.0f, 0.0f, -(fanZ * static_cast<float>(copies - 1) * 0.5f)
                                     + fanZ * static_cast<float>(copy)));
                    if (copy > 0) {
                        model = glm::translate(model, glm::vec3(
                            jitter.Signed(kCopyJitter * 0.5f),
                            jitter.Signed(kCopyJitter * 0.5f),
                            0.0f));
                    }
                } else if (copy > 0) {
                    model = glm::translate(model, glm::vec3(
                        jitter.Signed(kCopyJitter),
                        jitter.Signed(kCopyJitter),
                        jitter.Signed(kCopyJitter)));
                }

                // MC ItemTransform.apply, in its exact order:
                //   translate(display.translation)  — NOT scaled
                //   scale(display.scale)
                //   translate(-0.5, -0.5, -0.5)     — centring, IS scaled
                // Getting this backwards shrinks the ground lift by the scale
                // factor and leaves the model spinning about a corner.
                model = glm::translate(model, glm::vec3(0.0f, groundLift, 0.0f));
                model = glm::scale(model, glm::vec3(groundScale));
                model = glm::translate(model, glm::vec3(-0.5f, -0.5f, -0.5f));
                // Sprite meshes are authored in 0..16; bring them into the unit
                // cube the centring above assumes.
                if (preScale != 1.0f) {
                    model = glm::scale(model, glm::vec3(preScale));
                }

                g_renderBackend->SetUniformMat4(m_shader, "uMVP", viewProj * model);
                g_renderBackend->DrawIndexed(mesh, indexCount);
            }

            g_renderBackend->UnbindMesh();
    }

} // namespace Render
