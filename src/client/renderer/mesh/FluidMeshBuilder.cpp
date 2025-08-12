// File: src/client/renderer/mesh/FluidMeshBuilder.cpp
#include "FluidMeshBuilder.hpp"
#include "Mesher.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include <algorithm>

namespace Render {

    FluidMeshBuilder::FluidMeshBuilder(const FluidMeshConfig& config) : m_config(config) {
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

        // Get fluid type
        Game::BlockID fluidType = blocks.GetBlock(worldX, worldY, worldZ);

        if (!IsFluid(fluidType)) {
            return;
        }

        // Calculate fluid height and flow
        float fluidHeight = CalculateFluidHeight(blocks, worldX, worldY, worldZ, fluidType);
        FlowDirection flowDir = DetectFlowDirection(blocks, worldX, worldY, worldZ);

        // Get fluid tint
        glm::vec4 tint = GetFluidTint(fluidType);

        // Create top surface if exposed to air or different fluid
        Game::BlockID blockAbove = blocks.GetBlock(worldX, worldY + 1, worldZ);
        if (blockAbove == Game::BlockID::Air || !IsSameFluid(blockAbove, fluidType)) {
            CreateFluidTopSurface(fluidType, worldPos, fluidHeight, flowDir, tint, outMesh);
        }

        // Create side faces for exposed sides
        static const BlockFace sideFaces[] = {
            BlockFace::PositiveX, BlockFace::NegativeX,
            BlockFace::PositiveZ, BlockFace::NegativeZ
        };

        for (BlockFace face : sideFaces) {
            if (IsFluidFaceExposed(blocks, worldX, worldY, worldZ, face)) {
                CreateFluidSideFace(fluidType, worldPos, face, fluidHeight, tint, outMesh);
            }
        }

        // Create bottom face if exposed (rare, but possible)
        if (IsFluidFaceExposed(blocks, worldX, worldY, worldZ, BlockFace::NegativeY)) {
            AddFluidFace(fluidType, worldPos, BlockFace::NegativeY, 0.0f, tint, outMesh);
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

                    Game::BlockID blockId = blocks.GetBlock(worldX, worldY, worldZ);

                    if (IsFluid(blockId)) {
                        BuildFluidBlock(blocks, chunkPos, worldX, worldY, worldZ, outMesh);
                    }
                }
            }
        }
    }

    void FluidMeshBuilder::CreateFluidTopSurface(Game::BlockID fluidType, glm::vec3 blockPos,
                                                float height, FlowDirection flow,
                                                const glm::vec4& tint, SectionMesh& mesh) {

        // **FIXED**: Always use "still" texture for top surface as you requested
        std::string texturePath = GetFluidTextureForFace(fluidType, BlockFace::PositiveY);

        glm::vec4 uvRect;
        if (!GetFluidTextureUV(texturePath, uvRect)) {
            Log::Warning("Failed to get fluid texture UV for: %s", texturePath.c_str());
            return;
        }

        //Log::Debug("Creating fluid top surface with texture: %s", texturePath.c_str());

        // Create sloped or flat surface based on configuration and flow
        std::vector<Vertex> surfaceVerts;
        if (m_config.enableFluidSlopes && flow != FlowDirection::None) {
            surfaceVerts = CreateSlopedSurface(blockPos, height, flow, uvRect, tint);
        } else {
            surfaceVerts = CreateFluidQuad(blockPos, BlockFace::PositiveY, height, uvRect, tint);
        }

        // Add to translucent mesh (fluids are always translucent)
        if (surfaceVerts.size() == 4) {
            uint32_t baseIndex = static_cast<uint32_t>(mesh.translucentVerts.size());
            mesh.translucentVerts.insert(mesh.translucentVerts.end(), surfaceVerts.begin(), surfaceVerts.end());

            // Add indices for two triangles
            mesh.translucentIdxs.insert(mesh.translucentIdxs.end(), {
                baseIndex + 0, baseIndex + 1, baseIndex + 2,
                baseIndex + 0, baseIndex + 2, baseIndex + 3
            });

            //Log::Debug("Added fluid top surface: %zu vertices, %zu indices", surfaceVerts.size(), 6);
        }
    }

    void FluidMeshBuilder::CreateFluidSideFace(Game::BlockID fluidType, glm::vec3 blockPos, BlockFace face,
                                              float height, const glm::vec4& tint, SectionMesh& mesh) {
        AddFluidFace(fluidType, blockPos, face, height, tint, mesh);
    }

    void FluidMeshBuilder::AddFluidFace(Game::BlockID fluidType, glm::vec3 blockPos, BlockFace face,
                                       float height, const glm::vec4& tint, SectionMesh& mesh) {

        // **FIXED**: Use the face-specific texture method
        std::string texturePath = GetFluidTextureForFace(fluidType, face);
        glm::vec4 uvRect;
        if (!GetFluidTextureUV(texturePath, uvRect)) {
            Log::Warning("Failed to get fluid texture UV for face %d: %s", (int)face, texturePath.c_str());
            return;
        }

        //Log::Debug("Adding fluid face %d with texture: %s", (int)face, texturePath.c_str());

        // Create face quad
        std::vector<Vertex> faceVerts = CreateFluidQuad(blockPos, face, height, uvRect, tint);

        if (faceVerts.size() == 4) {
            uint32_t baseIndex = static_cast<uint32_t>(mesh.translucentVerts.size());
            mesh.translucentVerts.insert(mesh.translucentVerts.end(), faceVerts.begin(), faceVerts.end());

            mesh.translucentIdxs.insert(mesh.translucentIdxs.end(), {
                baseIndex + 0, baseIndex + 1, baseIndex + 2,
                baseIndex + 0, baseIndex + 2, baseIndex + 3
            });

            //Log::Debug("Added fluid face: %zu vertices, %zu indices", faceVerts.size(), 6);
        }
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
        Game::BlockID currentFluid = blocks.GetBlock(worldX, worldY, worldZ);

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

            // IBlockAccess handles bounds checking
            Game::BlockID neighbor = blocks.GetBlock(nx, worldY, nz);
            Game::BlockID neighborBelow = blocks.GetBlock(nx, worldY - 1, nz);

            // Flow toward air or empty space below
            if (neighbor == Game::BlockID::Air ||
                (neighborBelow == Game::BlockID::Air && neighbor != currentFluid)) {
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

    bool FluidMeshBuilder::IsFluidFaceExposed(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ, BlockFace face) {
        if (!m_config.enableFluidCulling) {
            return true;  // Always render if culling disabled
        }

        Game::BlockID currentFluid = blocks.GetBlock(worldX, worldY, worldZ);

        // Get neighbor position
        static const glm::ivec3 offsets[] = {
            { 0,  1,  0}, // PositiveY
            { 0, -1,  0}, // NegativeY
            { 0,  0,  1}, // PositiveZ
            { 0,  0, -1}, // NegativeZ
            { 1,  0,  0}, // PositiveX
            {-1,  0,  0}  // NegativeX
        };

        glm::ivec3 offset = offsets[static_cast<int>(face)];
        int neighborX = worldX + offset.x;
        int neighborY = worldY + offset.y;
        int neighborZ = worldZ + offset.z;

        // IBlockAccess handles cross-chunk access automatically
        Game::BlockID neighbor = blocks.GetBlock(neighborX, neighborY, neighborZ);

        // Face is exposed if neighbor is AIR
        if (neighbor == Game::BlockID::Air) {
            return true; // Always expose to air
        }

        // Cull face if neighbor is the same fluid type
        if (IsSameFluid(neighbor, currentFluid)) {
            return false; // Hide internal fluid-to-fluid interfaces
        }

        // **FIXED**: Don't show side face if there's a solid block adjacent
        // This prevents water/lava sides from showing against solid blocks
        return false;
    }

    bool FluidMeshBuilder::IsFluid(Game::BlockID blockId) const {
        return blockId == Game::BlockID::Water || blockId == Game::BlockID::Lava;
    }

    bool FluidMeshBuilder::IsSameFluid(Game::BlockID a, Game::BlockID b) const {
        return IsFluid(a) && a == b;
    }

    glm::vec4 FluidMeshBuilder::GetFluidTint(Game::BlockID fluidType) const {
        switch (fluidType) {
            case Game::BlockID::Water:
                return m_config.waterTint;
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

        // Adjust corner heights based on flow direction
        float h0 = baseHeight, h1 = baseHeight, h2 = baseHeight, h3 = baseHeight;

        switch (flow) {
            case FlowDirection::North:
                h0 = h3 = lowHeight; // North edge lower
                break;
            case FlowDirection::South:
                h1 = h2 = lowHeight; // South edge lower
                break;
            case FlowDirection::East:
                h1 = h0 = lowHeight; // East edge lower
                break;
            case FlowDirection::West:
                h2 = h3 = lowHeight; // West edge lower
                break;
            default:
                // No slope
                break;
        }

        // Create vertices with adjusted heights (counter-clockwise order)
        vertices[0] = Vertex(blockPos + glm::vec3(0, h0, 0), normal, glm::vec2(uvRect.x, uvRect.y), tint);
        vertices[1] = Vertex(blockPos + glm::vec3(1, h1, 0), normal, glm::vec2(uvRect.z, uvRect.y), tint);
        vertices[2] = Vertex(blockPos + glm::vec3(1, h2, 1), normal, glm::vec2(uvRect.z, uvRect.w), tint);
        vertices[3] = Vertex(blockPos + glm::vec3(0, h3, 1), normal, glm::vec2(uvRect.x, uvRect.w), tint);

        return vertices;
    }

    std::vector<Vertex> FluidMeshBuilder::CreateFluidQuad(glm::vec3 blockPos, BlockFace face, float height,
                                                         const glm::vec4& uvRect, const glm::vec4& tint) {
        std::vector<Vertex> vertices(4);

        // Use the same face normals as regular blocks
        static const glm::vec3 FACE_NORMALS[] = {
            { 0.0f,  1.0f,  0.0f}, // PositiveY (Top)
            { 0.0f, -1.0f,  0.0f}, // NegativeY (Bottom)
            { 0.0f,  0.0f,  1.0f}, // PositiveZ (Front)
            { 0.0f,  0.0f, -1.0f}, // NegativeZ (Back)
            { 1.0f,  0.0f,  0.0f}, // PositiveX (Right)
            {-1.0f,  0.0f,  0.0f}  // NegativeX (Left)
        };

        glm::vec3 normal = FACE_NORMALS[static_cast<int>(face)];

        // Create face vertices similar to regular blocks but with adjusted height
        switch (face) {
            case BlockFace::PositiveY: // Top face
                vertices[0] = Vertex(blockPos + glm::vec3(0, height, 0), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(1, height, 0), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(1, height, 1), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(0, height, 1), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                break;

            case BlockFace::NegativeY: // Bottom face
                vertices[0] = Vertex(blockPos + glm::vec3(0, 0, 1), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(1, 0, 1), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(1, 0, 0), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(0, 0, 0), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                break;

            case BlockFace::PositiveZ: // Front face
                vertices[0] = Vertex(blockPos + glm::vec3(0, 0, 1), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(0, height, 1), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(1, height, 1), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(1, 0, 1), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                break;

            case BlockFace::NegativeZ: // Back face
                vertices[0] = Vertex(blockPos + glm::vec3(1, 0, 0), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(1, height, 0), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(0, height, 0), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(0, 0, 0), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                break;

            case BlockFace::PositiveX: // Right face
                vertices[0] = Vertex(blockPos + glm::vec3(1, 0, 1), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(1, height, 1), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(1, height, 0), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(1, 0, 0), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                break;

            case BlockFace::NegativeX: // Left face
                vertices[0] = Vertex(blockPos + glm::vec3(0, 0, 0), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(0, height, 0), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(0, height, 1), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(0, 0, 1), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                break;
        }

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