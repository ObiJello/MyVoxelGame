// File: src/client/renderer/mesh/FluidMeshBuilder.hpp
#pragma once

#include "SectionMesh.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/core/Config.hpp"
#include <glm/glm.hpp>
#include <functional>

namespace Render {

    // Block face enum for fluid builder (matching Mesher.hpp)
    enum class BlockFace : int;

    // Fluid meshing configuration
    struct FluidMeshConfig {
        bool enableFluidSlopes = true;        // Create sloped fluid surfaces
        bool enableFluidCulling = true;       // Cull internal fluid faces
        float fluidHeight = 0.9f;            // Height of fluid surface (0.0-1.0)
        float flowAnimationSpeed = 1.0f;     // Speed of flow texture animation

        // Visual settings
        glm::vec4 waterTint{0.3f, 0.5f, 1.0f, 0.8f};  // Blue-ish water

        // WHITE, and it has to stay white. MC LiquidBlockRenderer.java:74 is
        // `int col = isLava ? 16777215 : BiomeColors.getAverageWaterColor(...)`
        // — 0xFFFFFF. The vertex colour on a fluid exists for water's biome
        // resolver; lava_still/lava_flow already carry their own colour and the
        // fragment shader multiplies the two. An orange tint on an orange
        // sprite crushes green to 40% and blue to 10%, which flattens every
        // yellow highlight and leaves a uniform red slab.
        glm::vec4 lavaTint{1.0f, 1.0f, 1.0f, 1.0f};

        bool enableFluidTransparency = true;
        float fluidAlpha = 0.8f;
    };

    // Fluid flow direction for surface sloping
    enum class FlowDirection {
        None,
        North, South, East, West,
        NorthEast, NorthWest, SouthEast, SouthWest
    };

    // Specialized mesh builder for fluid blocks (water, lava)
    class FluidMeshBuilder {
    public:
        // Biome water colour for a world position, supplied by the Mesher.
        //
        // Water is meshed on this separate path and never reaches the
        // per-block tint dispatch in AddBlockFace, so registering WATER in the
        // BlockColors table is not enough on its own — without this hook the
        // fluid keeps whatever fixed `waterTint` the config carries. MC has the
        // same split (LiquidBlockRenderer does its own getBlockTint call) and
        // resolves it through the identical WATER_COLOR_RESOLVER.
        std::function<glm::vec4(int, int, int)> waterTintProvider;

        explicit FluidMeshBuilder(const FluidMeshConfig& config = FluidMeshConfig{});

        // Build fluid geometry for one block using IBlockAccess
        void BuildFluidBlock(const Game::IBlockAccess& blocks, 
                           Game::Math::ChunkPos chunkPos,
                           int worldX, int worldY, int worldZ,
                           SectionMesh& outMesh);

        // Build fluid geometry for entire section (called from Mesher)
        void BuildFluidSection(const Game::IBlockAccess& blocks,
                             Game::Math::ChunkPos chunkPos,
                             int sectionY, SectionMesh& outMesh);

        // Update configuration
        void SetConfig(const FluidMeshConfig& config) { m_config = config; }
        const FluidMeshConfig& GetConfig() const { return m_config; }

    private:
        FluidMeshConfig m_config;

        // MC ItemBlockRenderTypes.LAYER_BY_FLUID registers WATER (and
        // FLOWING_WATER) as TRANSLUCENT and nothing else; getRenderLayer's
        // fallback is SOLID, so lava is a solid-layer block. Lava's sprite has
        // no alpha anywhere, so blending it only bought it a per-frame depth
        // sort against glass and water and an arbitrary order relative to both.
        static bool IsTranslucentFluid(Game::BlockID fluidType);

        // Appends one already-wound, already-shaded quad to whichever layer
        // IsTranslucentFluid puts this fluid in. Vertices are never shared
        // between quads — the translucent sorter keys off "quad k occupies
        // vertices 4k..4k+3" (see TranslucentSort.hpp).
        bool EmitFluidQuad(Game::BlockID fluidType, const std::vector<Vertex>& verts,
                           SectionMesh& mesh) const;

        // `backwardUpFace` emits a second, reverse-wound copy of the surface so
        // it stays visible from underneath once the translucent pass
        // back-face culls. MC does exactly this
        // (LiquidBlockRenderer.java:174 re-emits the four top vertices in
        // reverse order when FluidState.shouldRenderBackwardUpFace holds).
        void CreateFluidTopSurface(Game::BlockID fluidType, glm::vec3 blockPos,
                                  float height, FlowDirection flow,
                                  const glm::vec4& tint, SectionMesh& mesh,
                                  bool backwardUpFace);

        // MC FluidState.shouldRenderBackwardUpFace: true when any of the 3x3
        // cells around the block ABOVE is neither the same fluid nor a solid
        // render block — i.e. the surface could be looked at from below.
        bool ShouldRenderBackwardUpFace(const Game::IBlockAccess& blocks,
                                        int worldX, int worldY, int worldZ,
                                        Game::BlockID fluidType) const;

        // One horizontal side. MC emits it TWICE — forward, then the same four
        // vertices reversed — for every sprite but the water overlay
        // (LiquidBlockRenderer.java:264-269), so a fluid wall stays visible
        // from inside the fluid with back-face culling on.
        void CreateFluidSideFace(Game::BlockID fluidType, glm::vec3 blockPos, BlockFace face,
                                 float height, float bottomOffset,
                                 const glm::vec4& tint, SectionMesh& mesh);

        // The underside of the fluid volume, still sprite, full UV rect.
        void CreateFluidBottomFace(Game::BlockID fluidType, glm::vec3 blockPos,
                                   float bottomOffset, const glm::vec4& tint, SectionMesh& mesh);

        // Flow detection and surface calculation
        FlowDirection DetectFlowDirection(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ);
        float CalculateFluidHeight(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ, Game::BlockID fluidType);

        // MC LiquidBlockRenderer.shouldRenderFace plus the caller's
        // isFaceOccludedByNeighbor check, collapsed: draw the face unless the
        // neighbour is the same fluid or fully occludes it.
        bool ShouldRenderFluidFace(const Game::IBlockAccess& blocks,
                                   int worldX, int worldY, int worldZ,
                                   BlockFace face, Game::BlockID fluidType,
                                   float faceHeight) const;

        // MC `level.getBlockState(pos).getFluidState()`, reduced to the fluid's
        // own block id (Air = Fluids.EMPTY). This — not GetBlock — is what
        // every fluid decision in this class keys on, because a waterlogged
        // block, a kelp stalk and a coral fan all hold water while being
        // something else.
        Game::BlockID FluidAt(const Game::IBlockAccess& blocks,
                              int worldX, int worldY, int worldZ) const;

        // Helper functions. `IsFluid`/`IsSameFluid` take FLUID ids as returned
        // by FluidAt, never raw block ids from GetBlock.
        bool IsFluid(Game::BlockID blockId) const;
        bool IsSameFluid(Game::BlockID a, Game::BlockID b) const;

        // **NEW**: Face-specific texture selection
        std::string GetFluidTextureForFace(Game::BlockID fluidType, BlockFace face) const;


        glm::vec4 GetFluidTint(Game::BlockID fluidType, int worldX, int worldY, int worldZ) const;

        // Geometry creation
        std::vector<Vertex> CreateSlopedSurface(glm::vec3 blockPos, float height,
                                               FlowDirection flow, const glm::vec4& uvRect,
                                               const glm::vec4& tint);

        std::vector<Vertex> CreateFluidQuad(glm::vec3 blockPos, BlockFace face, float height,
                                           const glm::vec4& uvRect, const glm::vec4& tint);

        // Texture and UV helpers
        bool GetFluidTextureUV(const std::string& texturePath, glm::vec4& uvRect);
        glm::vec2 GetFlowTextureOffset(Game::BlockID fluidType) const;
    };

} // namespace Render