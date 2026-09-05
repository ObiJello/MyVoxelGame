// File: src/client/renderer/mesh/Mesher.hpp
#pragma once

#include "SectionMesh.hpp"
#include "FluidMeshBuilder.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/block/BlockModel.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "../texture/AtlasBuilder.hpp"
#include <glm/glm.hpp>
#include <array>
#include "common/world/level/DimensionId.hpp"
#include <mutex>
#include <atomic>
#include <cstdint>
#include <memory>

// **NEW**: Forward declaration to avoid circular dependency
namespace Game {
    class World;
}

namespace Client {
namespace Render {
    struct RegionSnapshot;   // MeshJobData.hpp — the 3x3x3 mesh input
}
}

namespace Render {

    // Face directions for block meshing
    enum class BlockFace : int {
        PositiveY = 0,  // Top (+Y)
        NegativeY = 1,  // Bottom (-Y)
        PositiveZ = 2,  // Front (+Z)
        NegativeZ = 3,  // Back (-Z)
        PositiveX = 4,  // Right (+X)
        NegativeX = 5   // Left (-X)
    };

    // Mesh generation configuration
    struct MeshConfig {
        bool enableAmbientOcclusion = true;
        bool enableFaceCulling = true;
        bool enableBiomeTinting = true;
        float biomeTintStrength = 1.0f;

        // Performance settings
        // Greedy face merging: coplanar full-cube faces with the same sprite
        // and identical corner colors collapse into one quad per maximal
        // rectangle (see Mesher::FlushGreedyQuads). Launch-time A/B kill
        // switch: OBEY_NO_GREEDY=1 disables merging at mesh time (a remesh —
        // i.e. a fresh world load — is needed for it to take effect).
        bool enableGreedyMeshing = true;
        int maxQuadsPerSection = 16384;    // Safety limit
    };

    // Render layer classification
    enum class RenderLayer {
        Opaque,      // Solid blocks (stone, dirt, wood)
        Cutout,      // Alpha-test blocks (leaves, grass, flowers)
        Translucent  // Blended blocks (glass, water, ice)
    };

    // Helper functions for render layer classification
    RenderLayer ClassifyBlock(Game::BlockID blockId);
    bool IsBlockOpaque(Game::BlockID blockId);
    bool IsBlockTranslucent(Game::BlockID blockId);

    // Core meshing class - turns block data into renderable geometry
    // An atlas sprite as the mesher sees it: its uv rect (x,y = min, z,w =
    // max) for building per-quad UVs, and its id in the atlas sprite table,
    // which a greedy-merged quad carries instead of the rect (TerrainVertex).
    struct SpriteRef {
        glm::vec4 rect{0.0f, 0.0f, 1.0f, 1.0f};
        uint16_t  id = 0;
    };

    class Mesher {
    public:
        // An inclusive block box in one dimension whose faces bake no ambient
        // occlusion (the occlusion wand). See "No ambient occlusion" below.
        struct AoExclusion {
            Game::DimensionId dimension = Game::DimensionId::Overworld;
            glm::ivec3 min{0};
            glm::ivec3 max{0};
        };
        // The cells in front of an axis-aligned portal surface, and the
        // direction from each into the surface. A face of such a cell that
        // lies ON the surface is never culled against the block behind it:
        // through the portal that block is not there, and a culled face
        // was a hole in the block beside the portal. See "portal faces".
        struct PortalFace {
            Game::DimensionId dimension = Game::DimensionId::Overworld;
            glm::ivec3 min{0};
            glm::ivec3 max{0};
            glm::ivec3 dir{0};
        };
        explicit Mesher(const MeshConfig& config = MeshConfig{});

        // **NEW**: Set world reference for cross-chunk neighbor access
        void SetWorld(Game::World* world);

        // Rebuild mesh for one 16x16x16 section (generic path: fills the block
        // cache through the IBlockAccess interface, then meshes from the cache)
        void BuildSectionMesh(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos, int sectionY, SectionMesh& outMesh);

        // Fast path for worker threads: fills the block cache directly from the
        // snapshot's flat arrays (memcpy for the interior and axis-aligned halo
        // planes) instead of ~10k virtual GetBlock calls per section. Produces
        // identical output to the IBlockAccess path over a SnapshotBlockAccess.
        void BuildSectionMesh(const Client::Render::RegionSnapshot& region,
                              Game::Math::ChunkPos chunkPos, int sectionY, SectionMesh& outMesh);

        // Convenience: rebuild entire chunk (all 24 sections)
        // DEPRECATED: Use BuildSectionMesh with IBlockAccess instead
        // void BuildChunkMesh(const Game::Chunk& chunk, ChunkMesh& outMesh);

        // Update configuration
        void SetConfig(const MeshConfig& config) { m_config = config; }
        const MeshConfig& GetConfig() const { return m_config; }

        // Get statistics from last mesh operation
        struct MeshStats {
            int facesGenerated = 0;
            int facesCulled = 0;
            int quadsGenerated = 0;
            // Greedy merging accounting for this section: how many merged
            // rectangles were emitted, and how many input quads they replaced
            // beyond themselves (i.e. quadsMergedAway quads never reached the
            // GPU). Final quad count = quadsGenerated - quadsMergedAway.
            int quadsMerged = 0;
            int quadsMergedAway = 0;
            float buildTimeMs = 0.0f;
        };
        const MeshStats& GetLastStats() const { return m_lastStats; }

        // Block property cache: avoids per-block registry lookups during meshing.
        // Populated once per thread from the static BlockRegistry, then reused for all
        // subsequent mesh builds on that thread. Public so free functions
        // (ClassifyBlock, IsBlockOpaque) in the same namespace can access them.
        struct CachedBlockProps {
            // Occlusion for the block's DEFAULT state. Correct on its own only
            // while every state of the block occludes the same way — see
            // `occlusionVariesByState`.
            bool isOpaque;
            // True when this block's states do NOT all agree about occlusion,
            // so the answer has to come from the voxel's state rather than from
            // this table.
            //
            // No vanilla block reaches this today, because the one family whose
            // occlusion genuinely varies — slabs, where `type=double` fills the
            // cell and `type=bottom` does not — is split across separate
            // BlockIDs here. It stops being empty the moment those collapse
            // back into a `type` property, and getting it wrong then would make
            // every double slab either leak faces or eat its neighbours'. The
            // flag is computed rather than assumed so that transition cannot be
            // silent: EnsureBlockPropsCache logs whatever lands in it.
            bool occlusionVariesByState = false;
            // The state-independent half of the occlusion test: an opaque
            // render layer, and not drawn by a BlockEntityRenderer. Combined
            // with the state's own IsFullCube() when occlusionVariesByState.
            bool opaqueMaterial = false;
            RenderLayer renderLayer;
            // Whether this block declares any blockstate properties. Lets
            // ProcessBlock skip the per-voxel state lookup entirely for the
            // ~99% of blocks that have none.
            bool hasStates;

            // MC BlockColors.createDefault, flattened. Vanilla dispatches the
            // tint on the BLOCK and treats tintIndex only as a filter inside
            // that block's resolver — which is why grass_block (tintindex 0)
            // takes the GRASS colormap while oak_leaves (also tintindex 0)
            // takes FOLIAGE. Dispatching on the index alone, as this mesher
            // used to, cannot express that distinction at all.
            enum class TintSource : uint8_t {
                None,      // no resolver registered -> untinted (MC returns -1)
                Biome,     // blend `tintChannel` over the biome grid
                Constant,  // fixed colour (spruce / birch leaves)
                FlowerBed, // tintIndex 0 untinted, otherwise grass
                StemAge,   // melon / pumpkin stem: colour computed from `age`
            };
            TintSource tintSource = TintSource::None;
            uint8_t    tintChannel = 0;            // BiomeChannel
            uint32_t   tintConstant = 0xFFFFFF;

            // MC HalfTransparentBlock.skipRendering: a face touching a
            // neighbour of the SAME block is dropped. Non-opaque blocks are
            // invisible to the ordinary occlusion test, so without this every
            // internal boundary inside a glass wall or an ice sheet is drawn
            // and blended — which reads as murky, visibly layered glass.
            //
            // Identity, not "both translucent": vanilla checks
            // `neighborState.is(this)`, so glass against stained glass keeps
            // both faces.
            bool cullsAgainstSelf = false;

            // Index into s_ctmUVs, or -1 for the great majority of blocks that
            // have no connected-texture variants. Kept as a slot rather than
            // 16 inline UV rects because CachedBlockProps is a per-thread array
            // over every BlockID, and 256 bytes each would cost ~300 KB a
            // thread to serve eighteen glass types.
            int16_t ctmSlot = -1;

            // MC `instanceof LeavesBlock`: every block whose class chain
            // reaches LeavesBlock (all `*_leaves`, including the azaleas,
            // cherry, pale oak and MangroveLeavesBlock). The identity that
            // LeavesBlock.skipRendering tests its NEIGHBOUR for.
            bool isLeaves = false;
            // MC LeavesBlock.skipRendering, evaluated for this block:
            //   !cutoutLeaves && neighbour instanceof LeavesBlock -> skip
            // i.e. true for a leaf block when leaves are not cutout (Fast
            // graphics), and also — engine extension — when the `cullLeaves`
            // option is on under Fancy. A face touching any `isLeaves`
            // neighbour is then dropped, whatever species it is.
            bool skipAgainstLeaves = false;
        };
        static constexpr size_t BLOCK_ID_COUNT = static_cast<size_t>(Game::BlockID::Count);
        static thread_local std::array<CachedBlockProps, BLOCK_ID_COUNT> s_blockPropsCache;
        Game::DimensionId m_dimension = Game::DimensionId::Overworld;
        // This dimension's "no AO" boxes, snapshotted by SetDimension once per
        // section build so the per-face test below touches no lock.
        std::vector<AoExclusion> m_aoExclusions;
        std::vector<PortalFace>  m_portalFaces;
        bool IsPortalFace(int worldX, int worldY, int worldZ, const glm::ivec3& dir) const {
            for (const PortalFace& f : m_portalFaces) {
                if (f.dir != dir) continue;
                if (worldX >= f.min.x && worldX <= f.max.x &&
                    worldY >= f.min.y && worldY <= f.max.y &&
                    worldZ >= f.min.z && worldZ <= f.max.z) return true;
            }
            return false;
        }
        bool AoDisabledAt(int worldX, int worldY, int worldZ) const {
            for (const AoExclusion& e : m_aoExclusions) {
                if (worldX >= e.min.x && worldX <= e.max.x &&
                    worldY >= e.min.y && worldY <= e.max.y &&
                    worldZ >= e.min.z && worldZ <= e.max.z) return true;
            }
            return false;
        }
        static thread_local bool s_blockPropsCacheValid;

        // The video options that change what a section mesh CONTAINS. They
        // live here as one packed atomic — NOT read from GameSettings —
        // because section builds run on worker threads, and GameSettings is
        // a string map the main thread writes to whenever the options screen
        // is touched. The main thread publishes a snapshot with
        // SetMeshOptions; every worker picks it up the next time
        // EnsureBlockPropsCache runs, which compares its thread-local
        // generation against the published one, rebuilds the block props
        // cache on a mismatch and copies the snapshot into the thread-local
        // s_activeMeshOptions the build reads. One acquire load per section
        // build, no per-block cost.
        //
        // Defaults match GameSettings' defaults (Fancy leaves, cullLeaves
        // off, smooth lighting on, biome blend 2) so a client that never
        // publishes still meshes MC-exact.
        struct MeshOptions {
            // MC cutoutLeaves (the old Fast/Fancy split).
            bool cutoutLeaves = true;
            // Engine `cullLeaves` option (the Cull Leaves mod's behaviour).
            bool cullLeaves   = false;
            // MC `ao` — Minecraft.useAmbientOcclusion(): off means every
            // face takes one flat light value (ModelBlockRenderer
            // renderModelFaceFlat) instead of the four-corner AO gradient.
            bool smoothLighting = true;
            // MC biomeBlendRadius 0..7 — ClientLevel.calculateBlockTint
            // averages a (2r+1)² square of biome colours per tinted quad.
            uint8_t biomeBlendRadius = 2;
            bool operator==(const MeshOptions&) const = default;
        };
        // Main thread only. Returns true when the snapshot actually changed,
        // which is the caller's cue to schedule a full remesh — an existing
        // mesh built under the old options is simply wrong under the new
        // ones (leaves in the other layer, faces present or missing, a
        // different light gradient, a different tint).
        static bool SetMeshOptions(MeshOptions options);

        // The block atlas was rebuilt (resource pack reload): every worker's
        // cached sprite rects and ids — s_faceUVCache, s_ctmUVs — belong to
        // the old atlas. Bumps the same generation SetMeshOptions does, so
        // each worker re-derives them on its next job. Every section must
        // remesh afterwards, as after SetMeshOptions.
        static void InvalidateAtlasCaches();

        // ── "No ambient occlusion" boxes (the occlusion wand) ────────────
        // Inclusive block boxes per dimension inside which faces bake no AO
        // darkening. The list is small and replaced whole; workers take a
        // shared snapshot per section build (AoExclusion is declared at the
        // top of the class, before the member that holds the snapshot).
        static void SetAoExclusions(Game::DimensionId dimension, const std::vector<AoExclusion>& boxes);
        static std::vector<AoExclusion> AoExclusionsFor(Game::DimensionId dimension);
        // ── Portal faces (see PortalFace) ────────────────────────────────
        static void SetPortalFaces(Game::DimensionId dimension, const std::vector<PortalFace>& faces);
        static std::vector<PortalFace> PortalFacesFor(Game::DimensionId dimension);
        // The level this mesher instance is building for (the worker sets it
        // per job); takes that dimension's exclusion snapshot at the same time.
        void SetDimension(Game::DimensionId dimension);

        // Greedy-debug coloring (Render Controls "Greedy Mesh View"): when on,
        // meshes are built with information colors instead of lighting —
        // merged rectangles on a red(1x1)->green(16x16) heat scale by area,
        // rule-ineligible quads (partial blocks, rotated UVs, AO gradients,
        // translucent) in dim blue-gray. Mesh-time state, so the toggle
        // triggers a remesh (ChunkRenderer::SetGreedyMeshDebug does).
        static void SetGreedyDebugColors(bool enable);
        static bool GreedyDebugColors();
        // Monotonic generation, bumped whenever the debug palette flips.
        // Meshes are stamped with it at build start (MeshBuildResult::
        // paletteGen); a parked chunk revived with a stale stamp re-dirties
        // in RestoreRetainedChunk, and an upload that raced the toggle
        // re-dirties in FinalizeSectionUpload — the toggle's RemeshAll only
        // reaches sections that are ACTIVE at that moment.
        static uint32_t GreedyPaletteGen();
        // Runtime master switch for greedy meshing (Render Controls checkbox;
        // OBEY_NO_GREEDY still forces it off at launch). Mesh-time state —
        // the caller triggers RemeshAll, same as the debug palette.
        static void SetGreedyEnabled(bool enable);
        static bool GreedyEnabled();
        // Packed debug-view colors, exposed for FluidMeshBuilder's still-fluid
        // merge pass so water plates read on the same red->green heat scale
        // (and the same ineligible blue-gray) as terrain, from one definition.
        static uint32_t GreedyDebugHeatColor(int area);
        static uint32_t GreedyDebugIneligibleColor();
        // Since-launch totals: greedy-ELIGIBLE quads that entered the merge
        // grids (terrain AND still-fluid), and the rectangles they became
        // (survivors count in both).
        static void GetGreedyTotals(uint64_t& eligibleIn, uint64_t& rectsOut);
        static MeshOptions GetMeshOptions();
        // Main thread only: reads cutoutLeaves / cullLeaves / ao /
        // biomeBlendRadius from Platform::g_gameSettings and publishes them.
        // Same return as SetMeshOptions. Call once after settings load and
        // again whenever any of those options is changed.
        static bool SyncMeshOptionsFromSettings();
        // Atlas rects for a block's connected-texture tiles, indexed by
        // Render::CTM::SlotFor(). One entry per participating block (see
        // CachedBlockProps::ctmSlot). Sized to CTM::kMaxVariants (64) so the
        // header need not pull in ConnectedTextures.hpp; 47 slots are used.
        static thread_local std::vector<std::array<SpriteRef, 64>> s_ctmUVs;

    private:
        MeshConfig m_config;
        mutable MeshStats m_lastStats;
        Game::World* m_world;  // World reference for cross-chunk access
        std::unique_ptr<FluidMeshBuilder> m_fluidBuilder;  // Fluid mesh builder

        void EnsureBlockPropsCache();

        // Published MeshOptions, packed so the set is one atomic: bit 0 =
        // cutoutLeaves, bit 1 = cullLeaves, bit 2 = smoothLighting, bits 4-7
        // = biomeBlendRadius. The generation is bumped AFTER the packed value
        // is stored (release) and read BEFORE it (acquire), so a worker that
        // observes a new generation also observes the options that came with
        // it.
        static std::atomic<uint16_t> s_meshOptionsPacked;
        static std::atomic<uint32_t> s_meshOptionsGeneration;
        // Generation the calling thread's s_blockPropsCache was built for.
        static thread_local uint32_t s_blockPropsCacheGeneration;
        // The snapshot the calling thread's builds read (AO, biome blend).
        // Written only by EnsureBlockPropsCache, alongside the cache.
        static thread_local MeshOptions s_activeMeshOptions;

        // Per-section block/opaque caches: 18x18x18 covering the 16x16x16 section
        // plus a 1-block border on all sides. Built once at the start of
        // BuildSectionMesh; ALL subsequent block reads (main loop, face culling,
        // AO, fluid neighbor sampling via CacheBlockAccess) index these arrays —
        // no virtual GetBlock calls remain on the meshing hot path.
        // Layout is [y][z][x] (x contiguous) to match SectionSnapshot's flat
        // array so the interior and Y/Z halo planes fill via memcpy.
        // Index with [localY+1][localZ+1][localX+1] where local coords are in [-1,16].
        Game::BlockID m_blockCache[18][18][18];
        bool m_opaqueCache[18][18][18];
        // Halo included, same 18^3 extent as the block cache.
        //
        // This used to be interior-only, on the reasoning that a block's own
        // state affects only its own model and neighbour lookups are for
        // occlusion and AO, which read block ids. Waterlogging breaks that:
        // whether a NEIGHBOUR holds water decides whether the shared fluid
        // face is culled, so the state of the halo is now load-bearing.
        // 16-bit, matching BlockStateIndex. As uint8_t this silently truncated
        // every state past 255 — 30 blocks, including every wall and
        // redstone_wire — so a waterlogged wall would have meshed as some
        // unrelated state of itself. 18^3 * 2 = 11.7 KB per mesher.
        Game::BlockStateIndex m_stateCache[18][18][18];
        // Fast path: every state in the 18^3 plane is literally index 0.
        // Deliberately NOT "all default" — since the MC state port, index 0 is
        // the block's FIRST state, not its default one (grass_block's default is
        // snowy=false, index 1). The flag only licenses substituting the literal
        // 0, which is what CachedState and DeriveWaterCache do.
        bool    m_stateCacheAllZero = true;
        // "This cell's fluid state is water" (MC BlockState.getFluidState),
        // derived once per section from the block and state caches. The fluid
        // mesher asks it for every voxel AND all six neighbours, which is far
        // too hot for a registry lookup per query — same reasoning as
        // m_opaqueCache, which exists for exactly the same access pattern.
        bool m_waterCache[18][18][18];
        // Biome grid for this section (with the blend margin), or null when
        // meshing straight off an IBlockAccess. See ResolveBiome.
        const Client::Render::RegionSnapshot* m_biomeSource = nullptr;
        // Only set on the direct-access path, where biomes come from the world.
        const Game::IBlockAccess* m_biomeAccess = nullptr;

        int m_sectionBaseWorldX;
        int m_sectionBaseWorldY;
        int m_sectionBaseWorldZ;
        void FillBlockCacheFromAccess(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos, int sectionY);
        void FillBlockCacheFromRegion(const Client::Render::RegionSnapshot& region,
                                        Game::Math::ChunkPos chunkPos, int sectionY);
        void DeriveOpaqueCache();
        // Also derives m_waterCache — both are one pass over the same 18^3
        // arrays, and both must be rebuilt together whenever either input is.
        void DeriveWaterCache();
        // Section-local coords in [0,16); the +1 shifts into halo indexing.
        Game::BlockStateIndex CachedState(int localX, int sectionLocalY, int localZ) const {
            return m_stateCacheAllZero
                       ? 0
                       : m_stateCache[sectionLocalY + 1][localZ + 1][localX + 1];
        }
        // Shared meshing body — reads only from m_blockCache/m_opaqueCache
        void BuildSectionMeshFromCache(Game::Math::ChunkPos chunkPos, int sectionY, SectionMesh& outMesh);
        bool GetCachedOpaque(int worldX, int worldY, int worldZ) const;
        Game::BlockID GetCachedBlock(int worldX, int worldY, int worldZ) const;

        // UV cache: thread-local so it persists across Mesher instances on the same
        // worker thread, avoiding ResolveTexture string allocs + atlas hash lookups
        // on every mesh rebuild.
        static thread_local std::unordered_map<const Game::FaceDef*, SpriteRef> s_faceUVCache;

        // Core meshing functions
        void ProcessBlock(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos,
                         int localX, int localY, int localZ,
                         int sectionY, Game::BlockID blockId, Game::BlockStateIndex stateIndex,
                         SectionMesh& mesh);

        // `stateIndex` is carried through because one tint resolver needs it:
        // MC colours melon/pumpkin stems by their AGE, so two stems of the same
        // BlockID are different colours. Reading it back out of `blocks` would
        // work but costs a world lookup per face for a value ProcessBlock
        // already has in hand.
        // Two-sided quads (TerrainVertex::kTwoSidedFlag). A zero-thickness
        // element with both faces on its thin axis (cross plants, seagrass
        // planes) draws the same rectangle twice, once per side, and the
        // second face's texture is the first's mirrored in u. ProcessBlock
        // runs both faces through AddBlockFace in CAPTURE mode, and when
        // the two agree (same corners, colours and sprite, uv mirrored
        // exactly) emits ONE quad in the kFacingTwoSided group, which the
        // renderer draws with back-face culling off while the fragment
        // shader mirrors u on the geometric back — pixel-identical, half
        // the vertices. Any disagreement emits both faces as before.
        struct FaceCapture {
            std::array<Vertex, 4> verts{};
            SpriteRef sprite{};
            glm::vec4 uvRect{};
            RenderLayer layer = RenderLayer::Opaque;
            Game::BlockID blockId = Game::BlockID::Air;
            bool valid = false;
        };
        void EmitCaptured(const FaceCapture& c, SectionMesh& mesh);
        bool EmitTwoSided(const FaceCapture& a, const FaceCapture& b, SectionMesh& mesh);

        void AddBlockFace(const Game::IBlockAccess& blocks,
                         const Game::BlockModel& model, const Game::Element& element,
                         Game::FaceDir faceDir, const Game::FaceDef& faceDef,
                         glm::vec3 blockPos, glm::vec3 faceNormal, Game::BlockID blockId,
                         Game::BlockStateIndex stateIndex,
                         int worldX, int worldY, int worldZ, RenderLayer layer, SectionMesh& mesh,
                         FaceCapture* capture = nullptr);

        // outFacing: the layer's per-quad facing list (SectionMesh), or null
        // for translucent, which keeps no groups.
        void GenerateQuad(const std::array<Vertex, 4>& quadVerts,
                         std::vector<TerrainVertex>& outVerts, std::vector<uint16_t>& outIndices,
                         std::vector<uint8_t>* outFacing);

        // Greedy face merging. AddBlockFace routes each opaque/cutout quad
        // through TryStashGreedyQuad; a quad that passes the (deliberately
        // strict) eligibility test parks in a per-(layer, face, plane) 16x16
        // grid instead of being emitted, and FlushGreedyQuads — called once
        // per section after the block loop — runs a classic 2D maximal-
        // rectangle merge per grid and emits one tiled quad per rectangle.
        // Everything ineligible (and every 1x1 survivor) is emitted through
        // GenerateQuad exactly as before, so the rendered image only ever
        // changes by having fewer, larger quads for the same pixels.
        // Returns false when the quad was NOT stashed and must be emitted.
        // baseColor: the four corners' tint * face shade WITHOUT ambient
        // occlusion (RGBA8, as faceVerts would carry with AO = 1); aoCode: each
        // corner's AO level 0..3 (1.0, 0.8, 0.6, 0.4) or 0xFF when the value is
        // not one of MC's four levels (sub-element blend) — never mergeable.
        bool TryStashGreedyQuad(const std::array<Vertex, 4>& faceVerts,
                                const SpriteRef& sprite,
                                const Game::Element& element,
                                const Game::FaceDef& faceDef,
                                BlockFace face, bool hasBlockOffset,
                                RenderLayer layer, Game::BlockID blockId,
                                const uint32_t (&baseColor)[4], const uint8_t (&aoCode)[4],
                                int worldX, int worldY, int worldZ);
        void FlushGreedyQuads(SectionMesh& outMesh);

        // Culling and optimization (uses m_opaqueCache for fast neighbor lookups)
        bool ShouldCullFace(int worldX, int worldY, int worldZ, BlockFace face);

        // Cross-chunk neighbor lookup via IBlockAccess
        Game::BlockID GetNeighborBlock(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ,
                                      BlockFace face);

        // Texture and material helpers
        bool GetTextureUV(const std::string& texturePath, SpriteRef& sprite);

        // **NEW**: Biome tinting methods for different tint indices
        // MC BiomeColors' four ColorResolvers.
        enum class BiomeChannel : uint8_t { Grass, Foliage, DryFoliage, Water };

        uint16_t  ResolveBiome(int worldX, int worldY, int worldZ) const;
        glm::vec4 BlendedBiomeTint(BiomeChannel channel,
                                   int worldX, int worldY, int worldZ) const;

        glm::vec4 CalculateGrassTint(Game::BlockID blockId, int worldX, int worldY, int worldZ);
        glm::vec4 CalculateFoliageTint(Game::BlockID blockId, int worldX, int worldY, int worldZ);
        glm::vec4 CalculateBiomeTint(Game::BlockID blockId, int worldX, int worldY, int worldZ);

        // Geometry helpers. `elemMin`/`elemMax` are the element's bounds in
        // [0,1] block-space (already divided by 16 from MC's pixel-space
        // from/to). `faceUv` is the element-face's `uv` field in MC pixel
        // units [0,16] — it selects a sub-rect of the atlas sprite `uvRect`.
        // Full-cube blocks pass elemMin=(0,0,0), elemMax=(1,1,1), faceUv=
        // (0,0,16,16) and behave exactly as before.
        std::array<Vertex, 4> CreateFaceVertices(glm::vec3 blockPos, BlockFace face,
                                              const glm::vec4& uvRect, const glm::vec4& tint,
                                              const glm::vec3& elemMin, const glm::vec3& elemMax,
                                              const glm::vec4& faceUv, int uvRotation);
        glm::vec3 GetFaceNormal(BlockFace face);

        // Minecraft-style per-vertex ambient occlusion
        // Returns a shade value 0.0-1.0 for a vertex corner based on 3 neighbor blocks
        float CalculateVertexAO(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ,
                                BlockFace face, int vertexIndex);

        // AO for a quad that does not fill its cell's face.
        //
        // MC ModelBlockRenderer.AmbientOcclusionFace: the four AO values are
        // properties of the CELL's face corners, and each vertex takes a
        // BILINEAR blend of them at its own position within that face
        // (AmbientOcclusionFace.calculate → the u/v weighting after
        // calculateShape). CalculateVertexAO alone hands corner k's value to
        // vertex k, which is only right when the quad spans the whole face.
        //
        // Every partial element gets this wrong without the blend, and it is
        // worst where two elements STACK on one side of a cell — a stair's
        // side and back are a slab quad below a step quad, and handing both
        // the same four cell-corner values puts the cell's bottom shading on
        // the step's bottom edge and its top shading on the slab's top edge,
        // i.e. a hard bright/dark seam across the middle of the block.
        //
        // `localPos` is the four vertices in block-local [0,1] space, in
        // CreateFaceVertices' emission order.
        void ComputeFaceAO(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ,
                           BlockFace face, const glm::vec3 (&localPos)[4], float (&outAO)[4]);

        // Minecraft directional face shading multiplier
        static float GetDirectionalShade(BlockFace face);

        // Which of the four in-plane neighbours of this face are the same
        // block, as a Render::CTM bitmask in TEXTURE space (left/right/top/
        // bottom of the sprite, not world axes). Drives connected glass.
        uint8_t ConnectedTextureMask(Game::BlockID blockId, BlockFace face,
                                     int worldX, int worldY, int worldZ) const;

        // **REMOVED**: WorldYToChunkY() - use Game::Math::WorldCoordinates instead

        // **UPDATED**: Use WorldCoordinates for coordinate conversion
        glm::vec3 LocalToWorldPos(const Game::Math::ChunkPos& chunkPos, int localX, int worldY, int localZ) const {
            return glm::vec3(
                chunkPos.x * Game::Math::CHUNK_SIZE_X + localX,
                static_cast<float>(worldY),
                chunkPos.z * Game::Math::CHUNK_SIZE_Z + localZ
            );
        }
    };

} // namespace Render