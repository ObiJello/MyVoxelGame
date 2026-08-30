// File: src/client/renderer/entity/BlockCubeEntityRenderer.cpp
#include "BlockCubeEntityRenderer.hpp"
#include "../core/Frustum.hpp"

#include "../backend/RenderBackend.hpp"
#include "../environment/EnvironmentState.hpp"
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

#ifdef HAS_VULKAN
#include "../backend/vulkan/VKBackend.hpp"
#endif

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <unordered_map>
#include <cmath>

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

    } // namespace

    bool BlockCubeEntityRenderer::Initialize() {
        if (!g_renderBackend) return false;

        // Same block shader ItemEntityRenderer uses, and the same Vulkan
        // caveat: the block shaders declare the Common UBO, so on VK they must
        // be created through the UBO-aware (portal) layout or pipeline
        // creation fails.
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
            Log::Warning("[BlockCubeEntityRenderer] failed to load block shader — "
                         "falling blocks and TNT will not render");
            return false;
        }

        m_cubeVB = g_renderBackend->CreateBuffer(
            BufferUsage::Vertex, kItemCubeMaxVerts * sizeof(ItemCubeVert),
            nullptr, BufferAccess::Streaming);
        m_cubeIB = g_renderBackend->CreateBuffer(
            BufferUsage::Index, kItemCubeMaxIdx * sizeof(uint32_t),
            nullptr, BufferAccess::Streaming);
        m_cubeMesh = g_renderBackend->CreateMesh(
            m_cubeVB, m_cubeIB, GetBlockVertexLayout());

        // ── The instanced path, if the backend has one ─────────────────────
        //
        // Every failure below is non-fatal and leaves m_instMesh invalid, which
        // Render reads as "use the per-entity loop". That is why the shader is
        // loaded through the plain path and not the Vulkan portal path: Vulkan
        // has no CreateInstancedMesh, so it never gets here anyway.
        {
            if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
                // Portal layout, like m_shader: block_vk.frag reads the
                // fog/environment CommonUBO at set=1. Resolves to
                // block_instanced_vk.vert.spv + block_vk.frag.spv.
                auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
                m_instShader = vk->CreateShaderFromFilesPortal(
                    "shaders/block_instanced.vert", "shaders/block.frag");
#endif
            } else {
                m_instShader = g_renderBackend->CreateShaderFromFiles(
                    "shaders/block_instanced.vert", "shaders/block.frag");
            }

            if (m_instShader != INVALID_SHADER) {
                m_instanceVB = g_renderBackend->CreateBuffer(
                    BufferUsage::Vertex, kMaxInstances * sizeof(Instance),
                    nullptr, BufferAccess::Streaming);
            }

            if (m_instanceVB != INVALID_BUFFER) {
                // Location 3: one vec4 per instance — xyz the world
                // translation, w the uniform scale (see Instance). The
                // divisor of 1 is what makes it per-instance.
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

#ifdef HAS_VULKAN
                if (g_renderBackend->GetType() == BackendType::Vulkan) {
                    // Pipelines bake the vertex input, so the shader must
                    // know both the per-vertex and per-instance layouts.
                    auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
                    vk->RegisterShaderVertexLayout(m_instShader, GetBlockVertexLayout());
                    vk->RegisterShaderInstanceLayout(m_instShader, instanceLayout);
                }
#endif
                m_instMesh = g_renderBackend->CreateInstancedMesh(
                    m_cubeVB, m_cubeIB, m_instanceVB,
                    GetBlockVertexLayout(), instanceLayout);
            }

            if (m_instMesh == INVALID_MESH) {
                Log::Info("[BlockCubeEntityRenderer] no instanced path — falling "
                          "back to one draw per entity");
            }
        }

        m_initialized = true;
        Log::Info("[BlockCubeEntityRenderer] initialized");
        return true;
    }

    void BlockCubeEntityRenderer::Shutdown() {
        if (!g_renderBackend) return;
        if (m_instMesh != INVALID_MESH)  { g_renderBackend->DestroyMesh(m_instMesh); m_instMesh = INVALID_MESH; }
        if (m_instanceVB != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(m_instanceVB); m_instanceVB = INVALID_BUFFER; }
        if (m_instShader != INVALID_SHADER){ g_renderBackend->DestroyShader(m_instShader); m_instShader = INVALID_SHADER; }
        if (m_cubeMesh != INVALID_MESH)  { g_renderBackend->DestroyMesh(m_cubeMesh); m_cubeMesh = INVALID_MESH; }
        if (m_cubeVB   != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(m_cubeVB); m_cubeVB = INVALID_BUFFER; }
        if (m_cubeIB   != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(m_cubeIB); m_cubeIB = INVALID_BUFFER; }
        if (m_shader   != INVALID_SHADER){ g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_initialized = false;
    }

    void BlockCubeEntityRenderer::SetRenderDistanceChunks(int chunks) {
        // chunks * 4 blocks — a quarter of the render distance, matching
        // ItemEntityRenderer. See its note for why this deliberately diverges
        // from MC's shouldRenderAtSqrDistance.
        m_cullRadius = static_cast<float>(std::max(chunks, 2) * 4);
    }

    void BlockCubeEntityRenderer::Render(const glm::mat4& projection,
                                         const glm::mat4& view,
                                         const glm::vec3& cameraPos,
                                         float partialTick) {
        if (!m_initialized || !g_renderBackend || !Client::g_clientMobManager) return;
        PROFILE_ZONE_N("BlockCubeRender");

        const glm::mat4 viewProj = projection * view;
        const float cullSq = m_cullRadius * m_cullRadius;

        // MC EntityRenderDispatcher -> EntityRenderer.shouldRender:62-74.
        // Distance alone was never MC's test; it also asks the frustum, which
        // is what stops a thousand primed TNT behind the camera from each
        // costing a draw, a matrix and a uniform slot. On Vulkan those slots
        // are finite, so this is a correctness contributor as well as a
        // performance one.
        const Frustum frustum = Frustum::FromMatrix(viewProj);

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
            m_instMesh != INVALID_MESH && m_instShader != INVALID_SHADER;
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
            g_renderBackend->SetUniformVec4(shader, "uPortalClipPlane", glm::vec4(0.0f));

            // A falling block IS in the world, so it fades into fog and dims at
            // night exactly like the terrain it fell from. Packing matches
            // ChunkRenderer's so the two fade at the same rate.
            const auto& env = EnvironmentState::Get().Frame();
            g_renderBackend->SetUniformFloat(shader, "uSkyBrightness", env.skyBrightness);
            g_renderBackend->SetUniformVec4(shader, "uFogColor",
                glm::vec4(env.fogColor, 1.0f));
            g_renderBackend->SetUniformVec4(shader, "uFogEnv",
                glm::vec4(env.fogEnvStart, env.fogEnvEnd, env.fogRdStart, env.fogRdEnd));
            g_renderBackend->SetUniformVec3(shader, "uCameraPos", cameraPos);

            // The instanced shader multiplies by the per-instance model matrix
            // itself, so it wants the view-projection alone. The per-entity
            // path folds model into uMVP inside its loop instead.
            if (useInstanced) {
                g_renderBackend->SetUniformMat4(shader, "uViewProj", viewProj);
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
        const auto gatherOne = [&](const Client::BlockEntityProxy& px,
                                   std::vector<DrawItem>& out) {
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
            }

            Instance inst{worldPos, 1.0f};
            bool whiteFlash = false;

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

            out.push_back(DrawItem{state, inst, whiteFlash});
        };

        std::vector<DrawItem>& items = m_items;
        items.clear();
        {
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
                    for (size_t i = begin; i < end; ++i) gatherOne(at(i), part);
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
                    gatherOne(at(i), items);
                }
            }
        }

        PROFILE_PLOT("BlockCube/Entities",
                     static_cast<int64_t>(Client::g_clientMobManager->All().size()));
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
                const uint32_t key = item.state.RawId();
                if (ranges.find(key) != ranges.end()) continue;

                BuildStateMesh(item.state, stateVerts, stateIdx);
                if (stateVerts.empty() || stateIdx.empty()) {
                    ranges.emplace(key, MeshRange{0, 0});
                    continue;
                }

                // Bounded by the shared streaming buffers. Dropping a state is
                // better than overrunning them; with one block model per state this
                // is only reachable with an implausible variety on screen at once.
                if (verts.size() + stateVerts.size() > kItemCubeMaxVerts ||
                    idx.size()   + stateIdx.size()   > kItemCubeMaxIdx) {
                    ranges.emplace(key, MeshRange{0, 0});
                    continue;
                }

                const uint32_t baseVertex = static_cast<uint32_t>(verts.size());
                const uint32_t firstIndex = static_cast<uint32_t>(idx.size());

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
            g_renderBackend->UpdateBuffer(m_cubeVB, 0,
                verts.size() * sizeof(ItemCubeVert), verts.data());
            g_renderBackend->UpdateBuffer(m_cubeIB, 0,
                idx.size() * sizeof(uint32_t), idx.data());
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
                    const auto it = ranges.find(item.state.RawId());
                    if (it == ranges.end() || it->second.indexCount == 0) continue;
                    const uint64_t key = (static_cast<uint64_t>(item.state.RawId()) << 1) |
                                         (item.whiteFlash ? 1u : 0u);
                    groups[key].push_back(item.instance);
                }

                uint32_t instanceBytesUsed = 0;
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
                    const uint32_t groupBytes =
                        static_cast<uint32_t>(insts.size() * sizeof(Instance));
                    if (instanceBytesUsed + groupBytes >
                        kMaxInstances * sizeof(Instance)) {
                        continue;   // buffer full — excess groups skip a frame
                    }
                    g_renderBackend->UpdateBuffer(m_instanceVB, instanceBytesUsed,
                        groupBytes, insts.data());
                    g_renderBackend->SetUniformVec4(shader, "uOverlayColor",
                                                    overlayOf(flash));
                    g_renderBackend->DrawIndexedInstanced(
                        m_instMesh, it->second.indexCount, it->second.firstIndex,
                        static_cast<uint32_t>(insts.size()), instanceBytesUsed);
                    instanceBytesUsed += groupBytes;
                }
            } else {
                for (const DrawItem& item : items) {
                    const auto it = ranges.find(item.state.RawId());
                    if (it == ranges.end() || it->second.indexCount == 0) continue;

                    // The same transform the instanced shader applies:
                    // translate, then a uniform scale of the model.
                    const glm::mat4 model =
                        glm::scale(glm::translate(glm::mat4(1.0f), item.instance.translate),
                                   glm::vec3(item.instance.scale));
                    g_renderBackend->SetUniformVec4(shader, "uOverlayColor",
                                                    overlayOf(item.whiteFlash));
                    g_renderBackend->SetUniformMat4(shader, "uMVP", viewProj * model);
                    g_renderBackend->DrawIndexed(m_cubeMesh, it->second.indexCount,
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
