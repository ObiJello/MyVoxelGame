// File: src/client/renderer/mesh/FluidMeshBuilder.hpp
//
// Port of MC's FluidRenderer (LiquidBlockRenderer before 26.x): the geometry
// for one fluid cell — a surface whose four corners sit at heights averaged
// from the neighbouring cells' fluid levels, side walls down to the floor,
// and an underside — with MC's own face culling, sprite selection and UV
// mapping:
//
//   * corner heights come from FluidState.getHeight over the 3x3 around the
//     cell (calculateAverageHeight), so a flowing stream slopes and a still
//     pool is flat at 8/9;
//   * the top uses the STILL sprite when the cell's flow vector is zero and
//     the FLOWING sprite rotated to the flow angle otherwise;
//   * sides sample the top-left quarter of the flowing sprite, cut to the
//     corner heights, or the water OVERLAY sprite against glass, ice and
//     leaves (HalfTransparentBlock / LeavesBlock);
//   * a face is skipped where the neighbour holds the same fluid, or where
//     the neighbour's (or the fluid's own block's) occlusion shape covers it
//     (isFaceOccludedByNeighbor / isFaceOccludedBySelf).
//
// Lighting: MC blends the lightmap per corner; this engine has no light
// data, so a face carries the directional shade only (1.0 up, 0.5 down, 0.8
// on Z faces, 0.6 on X faces — CardinalLighting's constants).
//
// The still-fluid greedy merge below is this engine's own: a flat, full-cell
// top or bottom (an ocean) parks in a per-plane grid and is emitted as tiled
// plates. Anything with a slope, a flow rotation or a non-uniform colour
// stays per block, which is exactly the set that cannot tile.
#pragma once

#include "SectionMesh.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/core/Config.hpp"
#include <glm/glm.hpp>
#include <functional>
#include <string>
#include <vector>

namespace Render {

    // Block face enum for fluid builder (matching Mesher.hpp)
    enum class BlockFace : int;

    struct FluidMeshConfig {
        // MC FluidRenderer: the vertex colour is the biome water colour
        // (BiomeColors.getAverageWaterColor, RGB) at alpha 1 — water's
        // translucency comes from the SPRITE's alpha (water_still.png is
        // 180/255 everywhere), not from the vertex. Multiplying a second
        // alpha in here made water noticeably clearer than vanilla's.
        glm::vec4 waterTint{1.0f, 1.0f, 1.0f, 1.0f};
        // WHITE, and it has to stay white: `tintSource` is null for lava's
        // fluid model, so its colour is -1 (0xFFFFFFFF). lava_still/lava_flow
        // carry their own colour and the fragment shader multiplies the two.
        glm::vec4 lavaTint{1.0f, 1.0f, 1.0f, 1.0f};
    };

    class FluidMeshBuilder {
    public:
        // Biome water colour for a world position, supplied by the Mesher.
        //
        // Water is meshed on this separate path and never reaches the
        // per-block tint dispatch in AddBlockFace, so registering WATER in the
        // BlockColors table is not enough on its own — without this hook the
        // fluid keeps whatever fixed `waterTint` the config carries. MC has the
        // same split (FluidRenderer resolves its own tintSource) and resolves
        // it through the identical WATER_COLOR_RESOLVER.
        std::function<glm::vec4(int, int, int)> waterTintProvider;

        // MC LightCoordsUtil.getLightCoords(level, pos) for a cell — the
        // packed light (block << 4 | sky << 20, FULL_BRIGHT for an
        // emissiveRendering state), supplied by the Mesher from its light
        // cache. Null: everything reads full sky.
        std::function<int(int, int, int)> lightProvider;

        // The level's cardinal lighting is MC's NETHER table (up/down 0.9),
        // set by the Mesher from its job's dimension.
        bool netherCardinalLight = false;

        explicit FluidMeshBuilder(const FluidMeshConfig& config = FluidMeshConfig{});

        // MC FluidRenderer.tesselate for the cell at (worldX, worldY, worldZ).
        // The cell's block state is read from `blocks`; the fluid is whatever
        // `BlockState.getFluidState()` says it holds — a water cell, a
        // waterlogged fence, a kelp stalk all render their water here.
        void BuildFluidBlock(const Game::IBlockAccess& blocks,
                             Game::Math::ChunkPos chunkPos,
                             int worldX, int worldY, int worldZ,
                             SectionMesh& outMesh);

        // Still-fluid greedy merging — the fluid half of the Mesher's greedy
        // pass (see the Greedy namespace rationale in Mesher.cpp). Flat
        // full-cell fluid TOP surfaces (and their flat underside quads) park
        // in a thread-local per-plane grid instead of being emitted, and the
        // flush merges maximal rectangles of cells whose surface height,
        // sprite rect and packed color match exactly. Legal for the animated
        // still sprites because their ATLAS RECT is constant — the animation
        // updates texels in place, so a tiled rect keeps animating.
        //
        // BeginGreedySection is called by the Mesher at the start of every
        // section build: it publishes the section base (grid coordinates are
        // section-local) and whether merging is on (m_config.enableGreedyMeshing
        // and OBEY_NO_GREEDY, both resolved by the Mesher so the two greedy
        // passes cannot disagree about the switch). It also defensively drops
        // any leftovers a previous build failed to flush, mirroring
        // Greedy::ResetThreadState.
        void BeginGreedySection(bool enabled, int baseWorldX, int baseWorldY, int baseWorldZ);

        // Called from Mesher::FlushGreedyQuads, once per section after the
        // block loop: emits one tiled plate per merged rectangle and every
        // 1x1 survivor verbatim, into the same SectionMesh layer vectors the
        // per-block path uses. The counters are the Mesher's own flush
        // accumulators — fluid merges ride the same MeshStats fields,
        // since-launch totals and Mesh/QuadsMerged plot as terrain merges.
        void FlushGreedyFluidQuads(SectionMesh& mesh, int& mergedQuads,
                                   int& cellsMerged, int& survivorQuads);

        // Update configuration
        void SetConfig(const FluidMeshConfig& config) { m_config = config; }
        const FluidMeshConfig& GetConfig() const { return m_config; }

    private:
        FluidMeshConfig m_config;

        // Still-fluid greedy state for the CURRENT section build, published by
        // BeginGreedySection. Instance members (not thread_local) because a
        // FluidMeshBuilder is owned by one Mesher, which is used by one worker
        // at a time; the merge grids themselves are thread_local in the .cpp,
        // same pattern as the Mesher's Greedy namespace.
        bool m_greedyEnabled = false;
        int  m_greedyBaseX = 0;
        int  m_greedyBaseY = 0;
        int  m_greedyBaseZ = 0;

        // Parks one flat full-cell fluid quad in the merge grid. `kind` is one
        // of the kGreedyKind* plane families in the .cpp (up-facing top,
        // down-facing top copy, volume bottom) — the three never merge with
        // each other. Returns false when the quad was NOT stashed (merging
        // off, cell outside the section, corner-color gradient, or the cell
        // already holds a quad) and must be emitted directly.
        bool TryStashGreedyFluidQuad(int kind, Game::FluidType fluidType,
                                     const std::vector<Vertex>& verts,
                                     uint16_t spriteId, uint32_t lightWord,
                                     int worldX, int worldY, int worldZ);

        // MC FluidRenderer.getLightCoords(level, pos): the brighter of the
        // cell and the one above, as a TerrainVertex light word.
        uint32_t FluidLight(int worldX, int worldY, int worldZ) const;

        // MC ItemBlockRenderTypes.LAYER_BY_FLUID registers WATER (and
        // FLOWING_WATER) as TRANSLUCENT and nothing else; getRenderLayer's
        // fallback is SOLID, so lava is a solid-layer block. Lava's sprite has
        // no alpha anywhere, so blending it only bought it a per-frame depth
        // sort against glass and water and an arbitrary order relative to both.
        static bool IsTranslucentFluid(Game::FluidType fluidType);

        // Appends one already-wound, already-shaded quad to whichever layer
        // IsTranslucentFluid puts this fluid in. Vertices are never shared
        // between quads — the translucent sorter keys off "quad k occupies
        // vertices 4k..4k+3" (see TranslucentSort.hpp).
        bool EmitFluidQuad(Game::FluidType fluidType, const std::vector<Vertex>& verts,
                           uint32_t lightWord, SectionMesh& mesh) const;

        // MC FluidRenderer.addFace: four corners in order, and optionally the
        // same four re-emitted reversed (`addBackFace`) so the quad survives
        // back-face culling from the other side. The reversed copy is a
        // SECOND quad with its own four vertices, never re-indexed — the
        // translucent re-sort rebuilds every quad's indices from one forward
        // template, so a facing carried in the index order would be lost.
        void AddFace(Game::FluidType fluidType, const Vertex (&corners)[4], bool addBackFace,
                     uint32_t lightWord, SectionMesh& mesh);

        // The sprites this fluid draws with (MC FluidModel: still, flowing,
        // and water's overlay). Rect x,y = min uv, z,w = max uv.
        struct SpriteRect {
            glm::vec4 rect{0.0f, 0.0f, 1.0f, 1.0f};
            uint16_t  id = 0;
            bool      valid = false;
        };
        SpriteRect LookupSprite(const std::string& texturePath) const;
        // `resonant`: the cell is Aurelith's resonant water (an always-water
        // block drawn as water with its own sprites — docs/fluids.md).
        SpriteRect StillSprite(Game::FluidType fluidType, bool resonant = false) const;
        SpriteRect FlowingSprite(Game::FluidType fluidType, bool resonant = false) const;
        SpriteRect OverlaySprite() const;   // water only; invalid for lava

        // MC FluidRenderer.getHeight(level, fluidType, pos, state, fluidState):
        // 1.0 when the same fluid continues above, the cell's own height when
        // it holds this fluid, 0 for a non-solid other block and -1 for a
        // solid one (which calculateAverageHeight then ignores).
        static float GetHeight(const Game::IBlockAccess& blocks, Game::FluidType fluidType,
                               const glm::ivec3& pos, Game::BlockState state,
                               const Game::FluidState& fluidState);
        static float GetHeight(const Game::IBlockAccess& blocks, Game::FluidType fluidType,
                               const glm::ivec3& pos);

        // MC calculateAverageHeight / addWeightedHeight.
        static float CalculateAverageHeight(const Game::IBlockAccess& blocks, Game::FluidType fluidType,
                                            float heightSelf, float height2, float height1,
                                            const glm::ivec3& cornerPos);

        // MC isFaceOccludedByState(direction, height, state): the occlusion
        // face `state` presents back toward the fluid covers the fluid's
        // `direction` face of the given height.
        static bool IsFaceOccludedByState(Game::Direction direction, float height,
                                          Game::BlockState state);
        static bool IsFaceOccludedByNeighbor(Game::Direction direction, float height,
                                             Game::BlockState neighborState);
        static bool IsFaceOccludedBySelf(Game::BlockState state, Game::Direction direction);
        // MC shouldRenderFace: not the same fluid next door, and not covered
        // by the fluid's own block.
        static bool ShouldRenderFace(const Game::FluidState& fluidState, Game::BlockState blockState,
                                     Game::Direction direction, const Game::FluidState& neighborFluid);

        // MC FluidState.shouldRenderBackwardUpFace(level, pos.above()).
        static bool ShouldRenderBackwardUpFace(const Game::IBlockAccess& blocks,
                                               int worldX, int worldY, int worldZ,
                                               Game::FluidType fluidType);

        // MC `relativeBlock instanceof HalfTransparentBlock || LeavesBlock`
        // — the neighbours a water side is drawn against with the overlay
        // sprite rather than the flowing one.
        static bool UsesWaterOverlay(Game::BlockID neighbour);

        glm::vec4 GetFluidTint(Game::FluidType fluidType, int worldX, int worldY, int worldZ) const;
    };

} // namespace Render
