// File: src/client/renderer/mesh/FluidMeshBuilder.cpp
#include "FluidMeshBuilder.hpp"
#include "Mesher.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include <algorithm>

namespace Render {

    // Minecraft directional face shading for fluids (same values as solid blocks)
    static float GetFluidDirectionalShade(BlockFace face) {
        switch (face) {
            case BlockFace::PositiveY: return 1.0f;
            case BlockFace::NegativeY: return 0.5f;
            case BlockFace::PositiveZ: return 0.8f;
            case BlockFace::NegativeZ: return 0.8f;
            case BlockFace::PositiveX: return 0.6f;
            case BlockFace::NegativeX: return 0.6f;
            default: return 1.0f;
        }
    }

    // Apply directional shade to a set of 4 vertices (gamma-space multiply like Minecraft)
    static void ApplyDirectionalShade(std::vector<Vertex>& verts, BlockFace face) {
        float shade = GetFluidDirectionalShade(face);
        for (auto& v : verts) {
            glm::vec4 c = v.GetColor();
            c.r *= shade;
            c.g *= shade;
            c.b *= shade;
            v.SetColor(c);
        }
    }

    // MC insets every fluid face 0.001 into its own block so it never
    // z-fights the neighbouring block's face (LiquidBlockRenderer.java:126).
    static constexpr float kFaceInset = 0.001f;

    // MC sprite.getU(f) / getV(f): f is a fraction of the SPRITE, not of the
    // atlas, so it interpolates between the sprite's own two edges.
    static inline float SpriteU(const glm::vec4& uvRect, float f) {
        return uvRect.x + f * (uvRect.z - uvRect.x);
    }
    static inline float SpriteV(const glm::vec4& uvRect, float f) {
        return uvRect.y + f * (uvRect.w - uvRect.y);
    }

    FluidMeshBuilder::FluidMeshBuilder(const FluidMeshConfig& config) : m_config(config) {
    }

    bool FluidMeshBuilder::IsTranslucentFluid(Game::BlockID fluidType) {
        return fluidType == Game::BlockID::Water;
    }

    Game::BlockID FluidMeshBuilder::FluidAt(const Game::IBlockAccess& blocks,
                                            int worldX, int worldY, int worldZ) const {
        // The fluid TYPE at a cell, encoded as the block id of the fluid
        // itself so the texture and tint lookups below need no second
        // vocabulary. Air means "no fluid" (MC's Fluids.EMPTY).
        //
        // Water is tested first and wins outright: nothing in MC is both
        // waterlogged and lava, and ContainsWater already covers plain water,
        // the 386 waterloggable blocks and the 5 always-water ones.
        if (blocks.ContainsWater(worldX, worldY, worldZ)) return Game::BlockID::Water;
        if (blocks.GetBlock(worldX, worldY, worldZ) == Game::BlockID::Lava) {
            return Game::BlockID::Lava;
        }
        return Game::BlockID::Air;
    }

    bool FluidMeshBuilder::EmitFluidQuad(Game::BlockID fluidType,
                                         const std::vector<Vertex>& verts,
                                         SectionMesh& mesh) const {
        if (verts.size() != 4) return false;

        const bool translucent = IsTranslucentFluid(fluidType);
        std::vector<Vertex>&   outVerts = translucent ? mesh.translucentVerts : mesh.opaqueVerts;
        std::vector<uint16_t>& outIdxs  = translucent ? mesh.translucentIdxs  : mesh.opaqueIdxs;

        // 16-bit index guard: section layer capped at 65,536 vertices.
        if (outVerts.size() + 4 > 65536) return false;

        const uint16_t base = static_cast<uint16_t>(outVerts.size());
        outVerts.insert(outVerts.end(), verts.begin(), verts.end());
        outIdxs.insert(outIdxs.end(), {
            static_cast<uint16_t>(base + 0), static_cast<uint16_t>(base + 1),
            static_cast<uint16_t>(base + 2),
            static_cast<uint16_t>(base + 0), static_cast<uint16_t>(base + 2),
            static_cast<uint16_t>(base + 3)
        });
        return true;
    }

    void FluidMeshBuilder::BuildFluidBlock(const Game::IBlockAccess& blocks,
                                         Game::Math::ChunkPos chunkPos,
                                         int worldX, int worldY, int worldZ,
                                         SectionMesh& outMesh) {

        // World position is already provided
        glm::vec3 worldPos(
            static_cast<float>(worldX),
            static_cast<float>(worldY),
            static_cast<float>(worldZ)
        );

        // MC `blockState.getFluidState()` — the FLUID at this cell, which is
        // not the same thing as the block at this cell. A waterlogged fence, a
        // kelp stalk and a coral fan all answer WATER while being something
        // else entirely (SectionCompiler.java:64 renders the liquid for any
        // block whose fluid state is non-empty).
        Game::BlockID fluidType = FluidAt(blocks, worldX, worldY, worldZ);

        if (!IsFluid(fluidType)) {
            return;
        }

        // Calculate fluid height and flow
        float fluidHeight = CalculateFluidHeight(blocks, worldX, worldY, worldZ, fluidType);
        FlowDirection flowDir = DetectFlowDirection(blocks, worldX, worldY, worldZ);

        // Get fluid tint
        glm::vec4 tint = GetFluidTint(fluidType, worldX, worldY, worldZ);

        // Face selection, following MC tesselate's renderUp/renderDown/renderN..E
        // block (LiquidBlockRenderer.java:90-95). The bottom is resolved first
        // because it decides the inset every side face starts from.
        const bool renderDown = ShouldRenderFluidFace(blocks, worldX, worldY, worldZ,
                                                      BlockFace::NegativeY, fluidType, fluidHeight);

        // MC: `float bottomOffs = renderDown ? 0.001F : 0.0F` — the sides drop
        // to the block floor when nothing is drawn there, and stop 0.001 short
        // of it when the bottom face is, so the two don't co-plane.
        const float bottomOffset = renderDown ? kFaceInset : 0.0f;

        // Create top surface if exposed to air or different fluid
        if (ShouldRenderFluidFace(blocks, worldX, worldY, worldZ,
                                  BlockFace::PositiveY, fluidType, fluidHeight)) {
            CreateFluidTopSurface(fluidType, worldPos, fluidHeight, flowDir, tint, outMesh,
                                  ShouldRenderBackwardUpFace(blocks, worldX, worldY, worldZ, fluidType));
        }

        // Create side faces for exposed sides
        static const BlockFace sideFaces[] = {
            BlockFace::PositiveX, BlockFace::NegativeX,
            BlockFace::PositiveZ, BlockFace::NegativeZ
        };

        for (BlockFace face : sideFaces) {
            if (ShouldRenderFluidFace(blocks, worldX, worldY, worldZ, face, fluidType, fluidHeight)) {
                CreateFluidSideFace(fluidType, worldPos, face, fluidHeight, bottomOffset, tint, outMesh);
            }
        }

        if (renderDown) {
            CreateFluidBottomFace(fluidType, worldPos, bottomOffset, tint, outMesh);
        }
    }

    void FluidMeshBuilder::BuildFluidSection(const Game::IBlockAccess& blocks,
                                            Game::Math::ChunkPos chunkPos,
                                            int sectionY, SectionMesh& outMesh) {
        // Calculate world Y bounds for this section
        int worldYBase = Config::MinY + sectionY * 16;

        // Process all blocks in section looking for fluids
        for (int localX = 0; localX < 16; ++localX) {
            for (int localY = 0; localY < 16; ++localY) {
                for (int localZ = 0; localZ < 16; ++localZ) {
                    int worldX = chunkPos.x * 16 + localX;
                    int worldY = worldYBase + localY;
                    int worldZ = chunkPos.z * 16 + localZ;

                    // Fluid STATE, so waterlogged blocks are picked up too —
                    // BuildFluidBlock would bail on them otherwise.
                    if (IsFluid(FluidAt(blocks, worldX, worldY, worldZ))) {
                        BuildFluidBlock(blocks, chunkPos, worldX, worldY, worldZ, outMesh);
                    }
                }
            }
        }
    }

    bool FluidMeshBuilder::ShouldRenderBackwardUpFace(const Game::IBlockAccess& blocks,
                                                      int worldX, int worldY, int worldZ,
                                                      Game::BlockID fluidType) const {
        // MC FluidState.java:63-72 — scan the 3x3 centred on the block above.
        // A neighbour that is neither the same fluid nor a solid render block
        // means there is a line of sight to the underside of this surface.
        //
        // "Same fluid" is the neighbour's FLUID state, so a waterlogged fence
        // overhead counts as water here exactly as a water block would.
        for (int ox = -1; ox <= 1; ++ox) {
            for (int oz = -1; oz <= 1; ++oz) {
                if (IsSameFluid(FluidAt(blocks, worldX + ox, worldY + 1, worldZ + oz),
                                fluidType)) {
                    continue;
                }
                const Game::BlockID id = blocks.GetBlock(worldX + ox, worldY + 1, worldZ + oz);
                const Game::Block& b = Game::BlockRegistry::Get(id);
                if (!b.opaque) return true;
            }
        }
        return false;
    }

    void FluidMeshBuilder::CreateFluidTopSurface(Game::BlockID fluidType, glm::vec3 blockPos,
                                                float height, FlowDirection flow,
                                                const glm::vec4& tint, SectionMesh& mesh,
                                                bool backwardUpFace) {

        // Still sprite, always. MC picks the FLOWING sprite here whenever
        // fluidState.getFlow() is non-zero and rotates its UVs by the flow
        // angle, but that vector is derived from per-block fluid levels this
        // engine does not store — DetectFlowDirection's air-neighbour guess is
        // not the same quantity and would rotate the surface for the wrong
        // reason. Left on the still sprite until fluid levels exist.
        std::string texturePath = GetFluidTextureForFace(fluidType, BlockFace::PositiveY);

        glm::vec4 uvRect;
        if (!GetFluidTextureUV(texturePath, uvRect)) {
            Log::Warning("Failed to get fluid texture UV for: %s", texturePath.c_str());
            return;
        }

        //Log::Debug("Creating fluid top surface with texture: %s", texturePath.c_str());

        // MC drops all four corner heights by 0.001 before emitting the up
        // face, so it never co-planes with a fluid surface in the block above.
        const float topHeight = height - kFaceInset;

        // Create sloped or flat surface based on configuration and flow
        std::vector<Vertex> surfaceVerts;
        if (m_config.enableFluidSlopes && flow != FlowDirection::None) {
            surfaceVerts = CreateSlopedSurface(blockPos, topHeight, flow, uvRect, tint);
        } else {
            surfaceVerts = CreateFluidQuad(blockPos, BlockFace::PositiveY, topHeight, uvRect, tint);
        }

        // Apply Minecraft directional face shading
        ApplyDirectionalShade(surfaceVerts, BlockFace::PositiveY);

        if (!EmitFluidQuad(fluidType, surfaceVerts, mesh)) {
            return;
        }

        // The underside of the surface, for looking up at it from in the
        // fluid. Both terrain passes back-face cull (as vanilla's do), so the
        // forward quad alone would vanish from below.
        //
        // The four vertices are DUPLICATED rather than re-indexed, exactly
        // as LiquidBlockRenderer.java:174 re-emits them. Translucent
        // sorting keys off "quad k occupies vertices 4k..4k+3"; sharing one
        // vertex block between two quads would break that invariant and
        // scramble the sort.
        //
        // The reversed facing is encoded in the VERTEX order — the same
        // four vertices walked the other way round the perimeter — and NOT
        // by reversing the indices. That distinction is load-bearing:
        // TranslucentSort rebuilds every quad's indices from one forward
        // template when it re-sorts (MC's MeshData.sortQuads does the same,
        // which is why LiquidBlockRenderer re-emits vertices rather than
        // indices). A quad that encoded its facing in the index order was
        // re-wound forward by the first re-sort and then back-face culled
        // away — water vanishing chunk by chunk as the sorter reached them.
        if (backwardUpFace) {
            const std::vector<Vertex> backVerts(surfaceVerts.rbegin(), surfaceVerts.rend());
            EmitFluidQuad(fluidType, backVerts, mesh);
        }
    }

    void FluidMeshBuilder::CreateFluidSideFace(Game::BlockID fluidType, glm::vec3 blockPos, BlockFace face,
                                               float height, float bottomOffset,
                                               const glm::vec4& tint, SectionMesh& mesh) {

        // **FIXED**: Use the face-specific texture method
        std::string texturePath = GetFluidTextureForFace(fluidType, face);
        glm::vec4 uvRect;
        if (!GetFluidTextureUV(texturePath, uvRect)) {
            Log::Warning("Failed to get fluid texture UV for face %d: %s", (int)face, texturePath.c_str());
            return;
        }

        // MC LiquidBlockRenderer.java:251-256. The side samples only the
        // TOP-LEFT QUARTER of the flow sprite — u over [0, 0.5], v over
        // [(1-height)*0.5, 0.5] — which is what puts flowing fluid on a block
        // side at 2x the still texture's scale. Mapping the full sprite
        // instead halves that scale, and anchoring v at the sprite top instead
        // of at the fluid surface makes a short fluid show the wrong slice.
        const float u0   = SpriteU(uvRect, 0.0f);
        const float u1   = SpriteU(uvRect, 0.5f);
        const float vTop = SpriteV(uvRect, (1.0f - height) * 0.5f);
        const float vBot = SpriteV(uvRect, 0.5f);

        // Per-direction corner walk, from MC's switch on Direction.Plane.HORIZONTAL.
        // (x0,z0) is the first top vertex and (x1,z1) the second; the order
        // differs per face precisely so all four wind counter-clockwise when
        // seen from outside the block. Each is inset 0.001 into the block.
        //
        // Without fluid levels every corner is at the same height, so hh0 and
        // hh1 are both `height`; the MC corner each one comes from is named in
        // the comments for whenever per-corner heights arrive.
        float x0, z0, x1, z1;
        switch (face) {
            case BlockFace::NegativeZ:  // NORTH — hh0 = NW, hh1 = NE
                x0 = 0.0f;             z0 = kFaceInset;
                x1 = 1.0f;             z1 = kFaceInset;
                break;
            case BlockFace::PositiveZ:  // SOUTH — hh0 = SE, hh1 = SW
                x0 = 1.0f;             z0 = 1.0f - kFaceInset;
                x1 = 0.0f;             z1 = 1.0f - kFaceInset;
                break;
            case BlockFace::NegativeX:  // WEST — hh0 = SW, hh1 = NW
                x0 = kFaceInset;       z0 = 1.0f;
                x1 = kFaceInset;       z1 = 0.0f;
                break;
            case BlockFace::PositiveX:  // EAST — hh0 = NE, hh1 = SE
                x0 = 1.0f - kFaceInset; z0 = 0.0f;
                x1 = 1.0f - kFaceInset; z1 = 1.0f;
                break;
            default:
                return;  // not a horizontal face
        }

        const glm::vec3 normal(0.0f, 1.0f, 0.0f);  // ignored by Vertex; MC also writes (0,1,0)
        std::vector<Vertex> faceVerts(4);
        faceVerts[0] = Vertex(blockPos + glm::vec3(x0, height,       z0), normal, glm::vec2(u0, vTop), tint);
        faceVerts[1] = Vertex(blockPos + glm::vec3(x1, height,       z1), normal, glm::vec2(u1, vTop), tint);
        faceVerts[2] = Vertex(blockPos + glm::vec3(x1, bottomOffset, z1), normal, glm::vec2(u1, vBot), tint);
        faceVerts[3] = Vertex(blockPos + glm::vec3(x0, bottomOffset, z0), normal, glm::vec2(u0, vBot), tint);

        // Apply Minecraft directional face shading
        ApplyDirectionalShade(faceVerts, face);

        if (!EmitFluidQuad(fluidType, faceVerts, mesh)) {
            return;
        }

        // MC re-emits the same four vertices reversed for every sprite except
        // the water overlay, so a fluid wall is double-sided and stays visible
        // once you are inside the fluid looking out. Same colour, same UVs.
        const std::vector<Vertex> backVerts(faceVerts.rbegin(), faceVerts.rend());
        EmitFluidQuad(fluidType, backVerts, mesh);
    }

    void FluidMeshBuilder::CreateFluidBottomFace(Game::BlockID fluidType, glm::vec3 blockPos,
                                                 float bottomOffset, const glm::vec4& tint,
                                                 SectionMesh& mesh) {

        std::string texturePath = GetFluidTextureForFace(fluidType, BlockFace::NegativeY);
        glm::vec4 uvRect;
        if (!GetFluidTextureUV(texturePath, uvRect)) {
            Log::Warning("Failed to get fluid texture UV for bottom face: %s", texturePath.c_str());
            return;
        }

        // MC LiquidBlockRenderer.java:181-193 — still sprite, full UV rect,
        // wound clockwise from above so the outward normal is -Y.
        const glm::vec3 normal(0.0f, 1.0f, 0.0f);
        std::vector<Vertex> faceVerts(4);
        faceVerts[0] = Vertex(blockPos + glm::vec3(0.0f, bottomOffset, 1.0f), normal, glm::vec2(uvRect.x, uvRect.w), tint);
        faceVerts[1] = Vertex(blockPos + glm::vec3(0.0f, bottomOffset, 0.0f), normal, glm::vec2(uvRect.x, uvRect.y), tint);
        faceVerts[2] = Vertex(blockPos + glm::vec3(1.0f, bottomOffset, 0.0f), normal, glm::vec2(uvRect.z, uvRect.y), tint);
        faceVerts[3] = Vertex(blockPos + glm::vec3(1.0f, bottomOffset, 1.0f), normal, glm::vec2(uvRect.z, uvRect.w), tint);

        ApplyDirectionalShade(faceVerts, BlockFace::NegativeY);
        EmitFluidQuad(fluidType, faceVerts, mesh);
    }

    // **NEW**: Face-specific texture selection as you requested
    std::string FluidMeshBuilder::GetFluidTextureForFace(Game::BlockID fluidType, BlockFace face) const {
        switch (fluidType) {
            case Game::BlockID::Water:
                if (face == BlockFace::PositiveY || face == BlockFace::NegativeY) {
                    // Top and bottom faces use "still" texture
                    return "block/water_still";
                } else {
                    // Side faces use "flow" texture
                    return "block/water_flow";
                }
                break;

            case Game::BlockID::Lava:
                if (face == BlockFace::PositiveY || face == BlockFace::NegativeY) {
                    // Top and bottom faces use "still" texture
                    return "block/lava_still";
                } else {
                    // Side faces use "flow" texture
                    return "block/lava_flow";
                }
                break;

            default:
                return "missingno";
        }
    }

    FlowDirection FluidMeshBuilder::DetectFlowDirection(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ) {
        if (!m_config.enableFluidSlopes) {
            return FlowDirection::None;
        }

        // Simple flow detection - check if there's air or lower fluid in adjacent blocks
        const Game::BlockID currentFluid = FluidAt(blocks, worldX, worldY, worldZ);

        // Check cardinal directions for flow
        struct FlowCheck {
            int dx, dz;
            FlowDirection dir;
        };

        static const FlowCheck checks[] = {
            { 0, -1, FlowDirection::North},
            { 0,  1, FlowDirection::South},
            { 1,  0, FlowDirection::East},
            {-1,  0, FlowDirection::West}
        };

        for (const auto& check : checks) {
            int nx = worldX + check.dx;
            int nz = worldZ + check.dz;

            // IBlockAccess handles bounds checking. Compared as FLUID states so
            // a waterlogged neighbour reads as "more of the same body of
            // water" rather than as an obstacle to flow away from.
            const Game::BlockID neighbor = FluidAt(blocks, nx, worldY, nz);
            const Game::BlockID neighborBelow = FluidAt(blocks, nx, worldY - 1, nz);
            const bool neighborEmpty =
                neighbor == Game::BlockID::Air &&
                blocks.GetBlock(nx, worldY, nz) == Game::BlockID::Air;
            const bool belowEmpty =
                neighborBelow == Game::BlockID::Air &&
                blocks.GetBlock(nx, worldY - 1, nz) == Game::BlockID::Air;

            // Flow toward air or empty space below
            if (neighborEmpty || (belowEmpty && neighbor != currentFluid)) {
                return check.dir;
            }
        }

        return FlowDirection::None;
    }

    float FluidMeshBuilder::CalculateFluidHeight(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ, Game::BlockID fluidType) {
        // For now, return standard fluid height
        // In a full implementation, this would calculate based on fluid level data
        return m_config.fluidHeight;
    }

    bool FluidMeshBuilder::ShouldRenderFluidFace(const Game::IBlockAccess& blocks,
                                                 int worldX, int worldY, int worldZ,
                                                 BlockFace face, Game::BlockID fluidType,
                                                 float faceHeight) const {
        // Get neighbor position
        static const glm::ivec3 offsets[] = {
            { 0,  1,  0}, // PositiveY
            { 0, -1,  0}, // NegativeY
            { 0,  0,  1}, // PositiveZ
            { 0,  0, -1}, // NegativeZ
            { 1,  0,  0}, // PositiveX
            {-1,  0,  0}  // NegativeX
        };

        const glm::ivec3 offset = offsets[static_cast<int>(face)];
        const int nx = worldX + offset.x;
        const int ny = worldY + offset.y;
        const int nz = worldZ + offset.z;

        // MC isNeighborSameFluid — the one cull that is unconditional, and the
        // only thing gating renderUp. Compares FLUID states, so a water block
        // beside a waterlogged fence sees one continuous body of water and
        // draws no wall between them, exactly as vanilla does.
        if (IsSameFluid(FluidAt(blocks, nx, ny, nz), fluidType)) {
            return false;  // Hide internal fluid-to-fluid interfaces
        }

        // IBlockAccess handles cross-chunk access automatically
        const Game::BlockID neighbor = blocks.GetBlock(nx, ny, nz);

        if (!m_config.enableFluidCulling) {
            return true;  // Always render if culling disabled
        }

        // MC's shouldRenderFace has a second clause, isFaceOccludedBySelf —
        // "does the fluid's OWN block cover this face" — which is what stops a
        // waterlogged bottom slab from drawing a water face along its solid
        // underside. It is deliberately NOT implemented here.
        //
        // It reads a per-FACE VoxelShape, and this engine stores one AABB per
        // state. For a stair that AABB is the union of the model's two
        // elements, i.e. a full cube (assets/models/block/stairs.json spans
        // 0..16 on every axis once unioned) — so applying MC's test to it
        // would delete EVERY water face on a waterlogged stair, which is far
        // worse than not applying it at all.
        //
        // Skipping it is visually free: the faces the test removes are by
        // definition the ones the block's own solid geometry covers, so the
        // depth buffer removes them anyway. The cost is hidden overdraw on the
        // handful of waterlogged blocks in a scene. Implement it for real when
        // blocks carry per-face occlusion shapes rather than a single box.

        // MC isFaceOccludedByNeighbor → isFaceOccludedByState. Only a
        // neighbour that FULLY occludes the shared face hides it: an empty
        // occlusion shape never does, and a full-cube one occludes every
        // direction except UP, which additionally needs the fluid to reach
        // y == 1. Render::IsBlockOpaque is the same "opaque material AND full
        // cube AND not a block entity" test the solid mesher culls with, which
        // is this engine's stand-in for a per-face occlusion shape.
        //
        // The old test was "anything that is not air culls the face", which
        // deleted the side of a fluid against glass, a torch, a slab, a fence
        // or another fluid — most visibly around lava lake edges in caves,
        // where it left you looking straight into the empty lava volume.
        if (!IsBlockOpaque(neighbor)) {
            return true;
        }
        if (face == BlockFace::PositiveY) {
            return faceHeight < 1.0f;
        }
        return false;
    }

    bool FluidMeshBuilder::IsFluid(Game::BlockID blockId) const {
        return blockId == Game::BlockID::Water || blockId == Game::BlockID::Lava;
    }

    bool FluidMeshBuilder::IsSameFluid(Game::BlockID a, Game::BlockID b) const {
        return IsFluid(a) && a == b;
    }

    glm::vec4 FluidMeshBuilder::GetFluidTint(Game::BlockID fluidType,
                                            int worldX, int worldY, int worldZ) const {
        switch (fluidType) {
            case Game::BlockID::Water: {
                // Biome water colour when the Mesher supplied a resolver. The
                // config's alpha is kept: MC's water colour carries no alpha of
                // its own (BiomeColors returns RGB), and transparency here is a
                // render-layer property rather than a biome one.
                if (waterTintProvider) {
                    glm::vec4 c = waterTintProvider(worldX, worldY, worldZ);
                    c.a = m_config.waterTint.a;
                    return c;
                }
                return m_config.waterTint;
            }
            case Game::BlockID::Lava:
                return m_config.lavaTint;
            default:
                return glm::vec4(1.0f);
        }
    }

    std::vector<Vertex> FluidMeshBuilder::CreateSlopedSurface(glm::vec3 blockPos, float height,
                                                             FlowDirection flow, const glm::vec4& uvRect,
                                                             const glm::vec4& tint) {
        std::vector<Vertex> vertices(4);
        glm::vec3 normal(0.0f, 1.0f, 0.0f);

        // Base height for all corners
        float baseHeight = height;
        float lowHeight = height; //* 0.7f; // Slightly lower on flow side

        // Corner heights, named as MC names them (LiquidBlockRenderer's
        // heightNorthWest / heightSouthWest / heightSouthEast / heightNorthEast).
        // North is -Z and West is -X, matching Mesher::FaceDirToBlockFace.
        float hNW = baseHeight, hSW = baseHeight, hSE = baseHeight, hNE = baseHeight;

        switch (flow) {
            case FlowDirection::North:
                hNW = hNE = lowHeight; // north (-Z) edge lower
                break;
            case FlowDirection::South:
                hSW = hSE = lowHeight; // south (+Z) edge lower
                break;
            case FlowDirection::East:
                hNE = hSE = lowHeight; // east (+X) edge lower
                break;
            case FlowDirection::West:
                hNW = hSW = lowHeight; // west (-X) edge lower
                break;
            default:
                // No slope
                break;
        }

        // MC's up-face corner walk: NW, SW, SE, NE — counter-clockwise seen
        // from above, so the outward normal is +Y. The old NW, NE, SE, SW
        // order wound the quad the other way, which made every fluid surface
        // face DOWN and left it to the reverse-wound backward-up copy to be
        // the only thing you could actually see from above.
        vertices[0] = Vertex(blockPos + glm::vec3(0, hNW, 0), normal, glm::vec2(uvRect.x, uvRect.y), tint);
        vertices[1] = Vertex(blockPos + glm::vec3(0, hSW, 1), normal, glm::vec2(uvRect.x, uvRect.w), tint);
        vertices[2] = Vertex(blockPos + glm::vec3(1, hSE, 1), normal, glm::vec2(uvRect.z, uvRect.w), tint);
        vertices[3] = Vertex(blockPos + glm::vec3(1, hNE, 0), normal, glm::vec2(uvRect.z, uvRect.y), tint);

        return vertices;
    }

    std::vector<Vertex> FluidMeshBuilder::CreateFluidQuad(glm::vec3 blockPos, BlockFace /*face*/, float height,
                                                         const glm::vec4& uvRect, const glm::vec4& tint) {
        // The flat top surface. Sides and the bottom have their own builders —
        // MC gives each of the three its own vertex walk and its own UV window,
        // and they cannot share one.
        //
        // Still sprite, full UV rect: MC's zero-flow branch is
        // u00 = getU(0), v00 = getV(0), u10 = getU(1), v01 = getV(1).
        const glm::vec3 normal(0.0f, 1.0f, 0.0f);
        std::vector<Vertex> vertices(4);
        vertices[0] = Vertex(blockPos + glm::vec3(0, height, 0), normal, glm::vec2(uvRect.x, uvRect.y), tint);
        vertices[1] = Vertex(blockPos + glm::vec3(0, height, 1), normal, glm::vec2(uvRect.x, uvRect.w), tint);
        vertices[2] = Vertex(blockPos + glm::vec3(1, height, 1), normal, glm::vec2(uvRect.z, uvRect.w), tint);
        vertices[3] = Vertex(blockPos + glm::vec3(1, height, 0), normal, glm::vec2(uvRect.z, uvRect.y), tint);
        return vertices;
    }

    bool FluidMeshBuilder::GetFluidTextureUV(const std::string& texturePath, glm::vec4& uvRect) {
        if (g_atlasBuilder) {
            AtlasUVRect atlasUV;
            if (g_atlasBuilder->GetUVRect(texturePath, atlasUV)) {
                uvRect = glm::vec4(atlasUV.uvMin.x, atlasUV.uvMin.y,
                                  atlasUV.uvMax.x, atlasUV.uvMax.y);
                return true;
            } else {
                Log::Warning("Failed to find texture '%s' in atlas", texturePath.c_str());
            }
        } else {
            Log::Warning("No atlas builder available for texture lookup");
        }

        // Fallback
        uvRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        return false;
    }

    glm::vec2 FluidMeshBuilder::GetFlowTextureOffset(Game::BlockID fluidType) const {
        // Simple time-based animation offset
        // In full implementation, this would be based on actual game time
        return glm::vec2(0.0f, 0.0f);
    }

} // namespace Render