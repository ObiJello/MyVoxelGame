// File: src/client/renderer/mesh/Mesher.cpp
#include "Mesher.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include <chrono>
#include <algorithm>

namespace Render {

    // Face normal vectors for each block face
    static const glm::vec3 FACE_NORMALS[] = {
        { 0.0f,  1.0f,  0.0f}, // PositiveY (Top)
        { 0.0f, -1.0f,  0.0f}, // NegativeY (Bottom)
        { 0.0f,  0.0f,  1.0f}, // PositiveZ (Front)
        { 0.0f,  0.0f, -1.0f}, // NegativeZ (Back)
        { 1.0f,  0.0f,  0.0f}, // PositiveX (Right)
        {-1.0f,  0.0f,  0.0f}  // NegativeX (Left)
    };

    // Face offset vectors for neighbor checking
    static const glm::ivec3 FACE_OFFSETS[] = {
        { 0,  1,  0}, // PositiveY
        { 0, -1,  0}, // NegativeY
        { 0,  0,  1}, // PositiveZ
        { 0,  0, -1}, // NegativeZ
        { 1,  0,  0}, // PositiveX
        {-1,  0,  0}  // NegativeX
    };

    Mesher::Mesher(const MeshConfig& config) : m_config(config), m_world(nullptr) {
        m_lastStats = {};
        // Create fluid mesh builder
        m_fluidBuilder = std::make_unique<FluidMeshBuilder>();
    }

    void Mesher::SetWorld(Game::World* world) {
        m_world = world;
    }

    void Mesher::BuildSectionMesh(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos, int sectionY, SectionMesh& outMesh) {
        auto startTime = std::chrono::high_resolution_clock::now();

        // Reset stats
        m_lastStats = {};

        // Clear output mesh
        outMesh.Clear();
        outMesh.chunkPos = chunkPos;
        outMesh.sectionY = sectionY;

        // Reserve space for estimated geometry
        outMesh.Reserve(1024); // Estimate ~1024 quads per section

        // No need to check section existence - IBlockAccess handles it

        // **UPDATED**: Process all blocks in this 16x16x16 section using world Y coordinates
        for (int localX = 0; localX < 16; ++localX) {
            for (int sectionLocalY = 0; sectionLocalY < 16; ++sectionLocalY) {
                for (int localZ = 0; localZ < 16; ++localZ) {
                    // **NEW**: Convert section-local Y to world Y coordinate
                    int worldY = sectionY * 16 + sectionLocalY + Config::MinY;
                    int worldX = chunkPos.x * 16 + localX;
                    int worldZ = chunkPos.z * 16 + localZ;

                    Game::BlockID blockId = blocks.GetBlock(worldX, worldY, worldZ);

                    if (blockId != Game::BlockID::Air) {
                        ProcessBlock(blocks, chunkPos, localX, worldY, localZ, sectionY, outMesh);
                    }
                }
            }
        }

        // Calculate build time
        auto endTime = std::chrono::high_resolution_clock::now();
        m_lastStats.buildTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    }

    // BuildChunkMesh removed - use BuildSectionMesh with IBlockAccess instead

    void Mesher::ProcessBlock(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos,
                             int localX, int worldY, int localZ,
                             int sectionY, SectionMesh& mesh) {

        // **UPDATED**: Use IBlockAccess with world coordinates
        int worldX = chunkPos.x * 16 + localX;
        int worldZ = chunkPos.z * 16 + localZ;
        Game::BlockID blockId = blocks.GetBlock(worldX, worldY, worldZ);
        if (blockId == Game::BlockID::Air) return;

        // Handle fluid blocks separately
        if (blockId == Game::BlockID::Water || blockId == Game::BlockID::Lava) {
            // Use FluidMeshBuilder for water and lava
            if (m_fluidBuilder) {
                m_fluidBuilder->BuildFluidBlock(blocks, chunkPos, worldX, worldY, worldZ, mesh);
                m_lastStats.facesGenerated++;  // Approximate count
            }
            return;
        }

        // Get block model
        const Game::BlockModel& model = Game::BlockRegistry::GetBlockModel(blockId);

        // Calculate world position for this block
        glm::vec3 worldPos = LocalToWorldPos(chunkPos, localX, worldY, localZ);

        // Process each element in the block model
        for (const auto& element : model.elements) {
            // Process each face of the element
            for (const auto& [faceDir, faceDef] : element.faces) {
                // Convert to our face enum
                BlockFace blockFace = static_cast<BlockFace>(static_cast<int>(faceDir));

                // Check if face should be culled
                if (m_config.enableFaceCulling) {
                    if (ShouldCullFace(blocks, worldX, worldY, worldZ, blockFace, blockId)) {
                        m_lastStats.facesCulled++;
                        continue;
                    }
                }

                // Get face normal
                glm::vec3 faceNormal = GetFaceNormal(blockFace);

                // Add this face to the mesh
                AddBlockFace(model, element, faceDir, faceDef, worldPos, faceNormal, blockId, worldX, worldY, worldZ, mesh);
                m_lastStats.facesGenerated++;
            }
        }
    }

    void Mesher::AddBlockFace(const Game::BlockModel& model, const Game::Element& element,
                             Game::FaceDir faceDir, const Game::FaceDef& faceDef,
                             glm::vec3 blockPos, glm::vec3 faceNormal, Game::BlockID blockId,
                             int worldX, int worldY, int worldZ, SectionMesh& mesh) {

        // Resolve texture path
        std::string texturePath = model.ResolveTexture(faceDef.textureRef);

        // Get UV coordinates from atlas
        glm::vec4 uvRect;
        if (!GetTextureUV(texturePath, uvRect)) {
            // Use error texture if not found
            texturePath = "missingno";
            GetTextureUV(texturePath, uvRect);
        }

        // **FIXED**: Calculate biome tint based on tintIndex
        glm::vec4 tintColor(1.0f, 1.0f, 1.0f, 1.0f); // Default white (no tint)

        if (m_config.enableBiomeTinting && faceDef.tintIndex >= 0) {
            if (faceDef.tintIndex == 0) {
                // Tint index 0 - usually foliage color (leaves, vines)
                tintColor = CalculateFoliageTint(blockId, worldX, worldY, worldZ);
            } else if (faceDef.tintIndex == 1) {
                // Tint index 1 - usually grass color (grass blocks)
                tintColor = CalculateGrassTint(blockId, worldX, worldY, worldZ);
            } else {
                // Other tint indices - you can add more specific tinting here
                tintColor = CalculateBiomeTint(blockId, worldX, worldY, worldZ);
            }
        }

        // Convert face direction to our BlockFace enum
        BlockFace blockFace = static_cast<BlockFace>(static_cast<int>(faceDir));

        // Create face vertices
        std::vector<Vertex> faceVerts = CreateFaceVertices(blockPos, blockFace, uvRect, tintColor);

        // Determine which render layer this goes into
        RenderLayer layer = ClassifyBlock(blockId);

        // Add to appropriate mesh layer
        switch (layer) {
            case RenderLayer::Opaque:
                GenerateQuad(faceVerts, mesh.opaqueVerts, mesh.opaqueIdxs);
                break;
            case RenderLayer::Cutout:
                GenerateQuad(faceVerts, mesh.cutoutVerts, mesh.cutoutIdxs);
                break;
            case RenderLayer::Translucent:
                GenerateQuad(faceVerts, mesh.translucentVerts, mesh.translucentIdxs);
                break;
        }

        m_lastStats.quadsGenerated++;
    }

    // **NEW**: Grass-specific tinting (tint index 1)
    glm::vec4 Mesher::CalculateGrassTint(Game::BlockID blockId, int worldX, int worldY, int worldZ) {
        // For now, return a simple green tint for grass
        // In a full implementation, this would sample from a colormap based on biome/temperature/humidity

        // Different blocks might have different base grass colors
        switch (blockId) {
            case Game::BlockID::Grass:
                return glm::vec4(0.5f, 0.7f, 0.3f, 1.0f); // Bright grass green
            default:
                return glm::vec4(0.5f, 0.7f, 0.3f, 1.0f); // Default grass green
        }
    }

    // **NEW**: Foliage-specific tinting (tint index 0)
    glm::vec4 Mesher::CalculateFoliageTint(Game::BlockID blockId, int worldX, int worldY, int worldZ) {
        // For now, return a simple green tint for foliage
        switch (blockId) {
            case Game::BlockID::OakLeaves:
                return glm::vec4(0.4f, 0.7f, 0.2f, 1.0f); // Oak leaves green
            case Game::BlockID::BirchLeaves:
                return glm::vec4(0.6f, 0.9f, 0.5f, 1.0f); // Brighter cherry green
            default:
                return glm::vec4(0.4f, 0.6f, 0.2f, 1.0f); // Default foliage green
        }
    }

    // **UPDATED**: General biome tinting
    glm::vec4 Mesher::CalculateBiomeTint(Game::BlockID blockId, int worldX, int worldY, int worldZ) {
        // This is where you'd implement full biome-based tinting
        // For now, return a neutral tint
        return glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    void Mesher::GenerateQuad(const std::vector<Vertex>& quadVerts,
                             std::vector<Vertex>& outVerts, std::vector<uint32_t>& outIndices) {
        if (quadVerts.size() != 4) {
            Log::Warning("GenerateQuad called with %zu vertices (expected 4)", quadVerts.size());
            return;
        }

        uint32_t baseIndex = static_cast<uint32_t>(outVerts.size());

        // Add vertices
        outVerts.insert(outVerts.end(), quadVerts.begin(), quadVerts.end());

        // **FIXED**: Correct triangle winding for counter-clockwise faces (viewed from outside)
        // Vertices are ordered: 0=bottom-left, 1=bottom-right, 2=top-right, 3=top-left
        outIndices.insert(outIndices.end(), {
            baseIndex + 0, baseIndex + 1, baseIndex + 2,  // First triangle: 0->1->2
            baseIndex + 0, baseIndex + 2, baseIndex + 3   // Second triangle: 0->2->3
        });
    }

    bool Mesher::ShouldCullFace(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ,
                               BlockFace face, Game::BlockID currentBlock) {
        // Get neighbor block using world coordinates
        Game::BlockID neighborBlock = GetNeighborBlock(blocks, worldX, worldY, worldZ, face);

        // Don't cull if neighbor is air
        if (neighborBlock == Game::BlockID::Air) {
            return false;
        }

        // Check if neighbor is opaque and covers this face
        return IsBlockOpaque(neighborBlock);
    }

    // **UPDATED**: Now supports cross-chunk neighbor lookup via IBlockAccess
    Game::BlockID Mesher::GetNeighborBlock(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ,
                                          BlockFace face) {
        glm::ivec3 offset = FACE_OFFSETS[static_cast<int>(face)];

        // Calculate neighbor position in world coordinates
        int neighborX = worldX + offset.x;
        int neighborY = worldY + offset.y;
        int neighborZ = worldZ + offset.z;

        // IBlockAccess handles cross-chunk boundaries automatically
        return blocks.GetBlock(neighborX, neighborY, neighborZ);
    }

    bool Mesher::GetTextureUV(const std::string& texturePath, glm::vec4& uvRect) {
        if (g_atlasBuilder) {
            AtlasUVRect atlasUV;
            if (g_atlasBuilder->GetUVRect(texturePath, atlasUV)) {
                uvRect = glm::vec4(atlasUV.uvMin.x, atlasUV.uvMin.y,
                                  atlasUV.uvMax.x, atlasUV.uvMax.y);
                return true;
            }
        }

        // Fallback to error texture
        uvRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        return false;
    }

    std::vector<Vertex> Mesher::CreateFaceVertices(glm::vec3 blockPos, BlockFace face,
                                                  const glm::vec4& uvRect, const glm::vec4& tint) {
        std::vector<Vertex> vertices(4);
        glm::vec3 normal = GetFaceNormal(face);

        // Small expansion to prevent gaps between blocks due to floating point precision
        const float BLOCK_EXPANSION = 0.001f;
        
        // **FIXED**: Corrected top face winding order to face upward instead of downward
        switch (face) {
            case BlockFace::PositiveY: // Top face (+Y)
                // When viewed from above, vertices should go counter-clockwise
                // Expand outward slightly to prevent gaps
                vertices[0] = Vertex(blockPos + glm::vec3(-BLOCK_EXPANSION, 1 + BLOCK_EXPANSION, 1 + BLOCK_EXPANSION), normal, glm::vec2(uvRect.x, uvRect.y), tint); // Front-left
                vertices[1] = Vertex(blockPos + glm::vec3(1 + BLOCK_EXPANSION, 1 + BLOCK_EXPANSION, 1 + BLOCK_EXPANSION), normal, glm::vec2(uvRect.z, uvRect.y), tint); // Front-right
                vertices[2] = Vertex(blockPos + glm::vec3(1 + BLOCK_EXPANSION, 1 + BLOCK_EXPANSION, -BLOCK_EXPANSION), normal, glm::vec2(uvRect.z, uvRect.w), tint); // Back-right
                vertices[3] = Vertex(blockPos + glm::vec3(-BLOCK_EXPANSION, 1 + BLOCK_EXPANSION, -BLOCK_EXPANSION), normal, glm::vec2(uvRect.x, uvRect.w), tint); // Back-left
                break;

            case BlockFace::NegativeY: // Bottom face (-Y)
                vertices[0] = Vertex(blockPos + glm::vec3(-BLOCK_EXPANSION, -BLOCK_EXPANSION, -BLOCK_EXPANSION), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(1 + BLOCK_EXPANSION, -BLOCK_EXPANSION, -BLOCK_EXPANSION), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(1 + BLOCK_EXPANSION, -BLOCK_EXPANSION, 1 + BLOCK_EXPANSION), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(-BLOCK_EXPANSION, -BLOCK_EXPANSION, 1 + BLOCK_EXPANSION), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                break;

            case BlockFace::PositiveZ: // Front face (+Z)
                vertices[0] = Vertex(blockPos + glm::vec3(-BLOCK_EXPANSION, -BLOCK_EXPANSION, 1 + BLOCK_EXPANSION), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(1 + BLOCK_EXPANSION, -BLOCK_EXPANSION, 1 + BLOCK_EXPANSION), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(1 + BLOCK_EXPANSION, 1 + BLOCK_EXPANSION, 1 + BLOCK_EXPANSION), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(-BLOCK_EXPANSION, 1 + BLOCK_EXPANSION, 1 + BLOCK_EXPANSION), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                break;

            case BlockFace::NegativeZ: // Back face (-Z)
                vertices[0] = Vertex(blockPos + glm::vec3(1 + BLOCK_EXPANSION, -BLOCK_EXPANSION, -BLOCK_EXPANSION), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(-BLOCK_EXPANSION, -BLOCK_EXPANSION, -BLOCK_EXPANSION), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(-BLOCK_EXPANSION, 1 + BLOCK_EXPANSION, -BLOCK_EXPANSION), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(1 + BLOCK_EXPANSION, 1 + BLOCK_EXPANSION, -BLOCK_EXPANSION), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                break;

            case BlockFace::PositiveX: // Right face (+X)
                vertices[0] = Vertex(blockPos + glm::vec3(1 + BLOCK_EXPANSION, -BLOCK_EXPANSION, 1 + BLOCK_EXPANSION), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(1 + BLOCK_EXPANSION, -BLOCK_EXPANSION, -BLOCK_EXPANSION), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(1 + BLOCK_EXPANSION, 1 + BLOCK_EXPANSION, -BLOCK_EXPANSION), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(1 + BLOCK_EXPANSION, 1 + BLOCK_EXPANSION, 1 + BLOCK_EXPANSION), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                break;

            case BlockFace::NegativeX: // Left face (-X)
                vertices[0] = Vertex(blockPos + glm::vec3(-BLOCK_EXPANSION, -BLOCK_EXPANSION, -BLOCK_EXPANSION), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(-BLOCK_EXPANSION, -BLOCK_EXPANSION, 1 + BLOCK_EXPANSION), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(-BLOCK_EXPANSION, 1 + BLOCK_EXPANSION, 1 + BLOCK_EXPANSION), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(-BLOCK_EXPANSION, 1 + BLOCK_EXPANSION, -BLOCK_EXPANSION), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                break;
        }

        return vertices;
    }

    glm::vec3 Mesher::GetFaceNormal(BlockFace face) {
        return FACE_NORMALS[static_cast<int>(face)];
    }

    uint8_t Mesher::CalculateAmbientOcclusion(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ,
                                             BlockFace face, int vertexIndex) {
        if (!m_config.enableAmbientOcclusion) {
            return 255; // Full brightness
        }

        // Simple AO implementation - would be more sophisticated in full version
        // For now, just return full brightness
        return 255;
    }

    // Render layer classification functions
    RenderLayer ClassifyBlock(Game::BlockID blockId) {
        const Game::Block& block = Game::BlockRegistry::Get(blockId);

        // Check for transparent blocks first
        if (block.isTransparent) {
            // Distinguish between cutout and translucent
            switch (blockId) {
                case Game::BlockID::OakLeaves:
                case Game::BlockID::BirchLeaves:
                case Game::BlockID::CherryLeaves:
                case Game::BlockID::Spawner:
                    return RenderLayer::Cutout;

                case Game::BlockID::Glass:
                case Game::BlockID::Ice:
                case Game::BlockID::Water:
                case Game::BlockID::Lava:
                    return RenderLayer::Translucent;

                default:
                    return block.opaque ? RenderLayer::Opaque : RenderLayer::Cutout;
            }
        }

        return RenderLayer::Opaque;
    }

    bool IsBlockOpaque(Game::BlockID blockId) {
        const Game::Block& block = Game::BlockRegistry::Get(blockId);
        return block.opaque;
    }

    bool IsBlockTranslucent(Game::BlockID blockId) {
        RenderLayer layer = ClassifyBlock(blockId);
        return layer == RenderLayer::Translucent;
    }

} // namespace Render