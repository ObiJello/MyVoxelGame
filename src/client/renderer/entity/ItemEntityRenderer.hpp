// File: src/client/renderer/entity/ItemEntityRenderer.hpp
//
// Draws dropped items lying in the world — MC's ItemEntityRenderer.
//
// Geometry comes from the same builders the first-person hand uses
// (Render::BuildBlockModelMesh for block items, HeldItemSpriteMesh for sprite
// items), so a cobblestone on the ground and a cobblestone in your hand are the
// same model. What differs is the transform: world space instead of view space,
// the item model's `display.ground` scale, and MC's bob + spin.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "../viewmodel/ItemMeshBuilder.hpp"   // ItemCubeVert
#include "common/entity/Item.hpp"
#include "common/entity/ItemEntity.hpp"
#include <glm/glm.hpp>
#include <vector>

struct Frustum;   // client/renderer/core/Frustum.hpp (global)

namespace Render {

    // Resource pack reload: the block-item meshes carry atlas UVs.
    void ClearBlockItemMeshCacheForReload();

    class ItemEntityRenderer {
    public:
        bool Initialize();
        void Shutdown();

        // Draw every entity in Client::g_itemEntityManager.
        //
        // `partialTick` (0..1) is how far through the current 50 ms client tick
        // this frame is, used to blend each entity's previous-tick position
        // with its current one. Without it items would jump once per tick
        // rather than moving smoothly — the same reason PlayerRenderer takes
        // it.
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, float partialTick);

        // Last Render's tally, for the portal diagnostics (OBEY_PORTAL_DIAG).
        // /morph: one dropped item at a world position, bobbing and
        // spinning as a drop does (`bobOffs` is the drop's phase). No cull.
        void RenderSingle(const Game::ItemStack& stack, const glm::dvec3& worldPos,
                          float ageTicks, float bobOffs,
                          const glm::mat4& projection, const glm::mat4& view,
                          const glm::vec3& cameraPos);

        // MC ItemEntityRenderer.renderMultipleFromCount under a caller's pose
        // — the vault's display item (VaultRenderer) and the ominous item
        // spawner: the stack's copies centred at `worldPos`, turned `yRotDeg`
        // about Y and scaled by `scale` (the item model's display.ground
        // inside that), with no bob and no hover lift, lit by `light` (a
        // lightmap colour, EntityEnvironment::LightColor). No cull.
        void RenderCluster(const Game::ItemStack& stack, const glm::dvec3& worldPos, float yRotDeg,
                           float scale, const glm::vec3& light,
                           const glm::mat4& projection, const glm::mat4& view,
                           const glm::vec3& cameraPos);

        struct Tally { int entities = 0, drawn = 0, cullDistance = 0, cullFrustum = 0, cullSection = 0; };
        const Tally& LastTally() const { return m_tally; }

        // Items draw out to a QUARTER of the render distance, in chunks:
        //   8 chunks  -> 2 chunks -> 32 blocks
        //   16 chunks -> 4 chunks -> 64 blocks
        //   32 chunks -> 8 chunks -> 128 blocks
        // i.e. simply `chunks * 4` blocks, since a quarter of a chunk-count
        // times 16 blocks per chunk is the same thing.
        //
        // Call it once per frame with the EFFECTIVE render distance (the
        // client setting already clamped by the server's view distance) —
        // items the server never sent cannot be drawn at any range, so scaling
        // off the raw client setting would promise more than the wire gives.
        void SetRenderDistanceChunks(int chunks, float entityDistanceScaling = 1.0f);

    private:
        // Draw one item at a world position with MC's bob + spin + ground
        // transform. Shared by the entities lying in the world and by the
        // pickup animations flying into a player, so the two can never drift
        // apart visually.
        //
        // `ageTicks` drives bob and spin; a pickup animation passes the frozen
        // age it was captured with, which is what keeps a collected item from
        // continuing to rotate as it flies.
        //
        // The state that does NOT change between items — shader, fog
        // environment, camera, clip plane — is bound once by the first item
        // drawn in a pass; `PassState` carries what the previous item left
        // bound so the per-item cost is the MVP, and the pipeline/texture/
        // alpha-test only when they actually differ (block vs sprite item).
        struct PassState {
            bool          begun     = false;
            bool          lastBlock = false;
            TextureHandle lastTex   = INVALID_TEXTURE;
        };
        // `renderPos` is render-space (Render::ToRender of the item's
        // interpolated world position); `cameraPos` stays world-space.
        // A caller-posed draw (RenderCluster): the spin is given, the bob
        // and hover lift are off, the light is given.
        struct ClusterPose {
            float     yRotRad = 0.0f;
            glm::vec3 light{1.0f};
        };
        void DrawItem(const Game::ItemStack& stack, const glm::vec3& renderPos,
                      float ageTicks, float bobOffs,
                      const glm::mat4& viewProj, const glm::vec3& cameraPos,
                      PassState& pass, float scale = 1.0f,
                      const ClusterPose* cluster = nullptr);

        // MC OminousItemSpawnerRenderer, for every ominous item spawner in
        // the client's mob store: grows in over its first 50 ticks, spins 40
        // degrees a tick, full bright. Culled like a dropped item.
        bool HasOminousItemSpawners() const;
        void DrawOminousItemSpawners(const glm::mat4& viewProj, const glm::vec3& cameraPos,
                                     float partialTick, const ::Frustum& frustum, PassState& pass);

        bool m_initialized = false;

        ShaderHandle  m_shader       = INVALID_SHADER;
        TextureHandle m_dummyTexture = INVALID_TEXTURE;

        // Streaming scratch buffers for the block-item path, reused across all
        // entities in a frame — one item's mesh is uploaded, drawn, and
        // overwritten by the next.
        BufferHandle m_cubeVB   = INVALID_BUFFER;
        BufferHandle m_cubeIB   = INVALID_BUFFER;
        MeshHandle   m_cubeMesh = INVALID_MESH;

        // The cull radius in BLOCKS, driven by SetRenderDistanceChunks.
        //
        // DELIBERATE DIVERGENCE FROM VANILLA, recorded because the old value
        // was a faithful port and someone will otherwise "fix" this back.
        // MC Entity.shouldRenderAtSqrDistance (Entity.java:1996) derives the
        // cutoff from the HITBOX:
        //
        //     size = boundingBox.getSize()            // mean of the 3 extents
        //     size *= 64.0 * viewScale
        //     return distanceSq < size * size
        //
        // A 0.25³ item gives 0.25 * 64 = 16 blocks * viewScale, and viewScale
        // is `clamp(renderDistance / 8, 1.0, 2.5) * entityDistanceScaling`
        // (LevelRenderer.java:754) — so vanilla shows items at 16 blocks on an
        // 8-chunk view and 40 at 20+, before the Entity Distance slider.
        //
        // This engine scales off the view distance directly instead: items are
        // visible to a quarter of it. Still more generous than vanilla at every
        // setting (32 blocks where vanilla gives 16 at 8 chunks) and it is a
        // look choice, not an oversight.
        //
        // Floored at one chunk so a very small view distance cannot cull items
        // out of arm's reach.
        //
        // The Entity Distance option (MC entityDistanceScaling) multiplies
        // the result, so the slider still governs items the way it governs
        // everything else; only the base rule diverges.
        static constexpr float kMinRenderDistance = 16.0f;

        // Default corresponds to an 8-chunk view, so the first frame before
        // SetRenderDistanceChunks lands is already sensible.
        float m_maxRenderDistance = 8.0f * 4.0f;
        Tally m_tally;
    };

} // namespace Render
