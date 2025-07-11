// File: src/render/mesh/Mesher.cpp
#include "Mesher.hpp"
#include "FluidMeshBuilder.hpp"
#include "../../engine/block/BlockRegistry.hpp"
#include "../../core/Log.hpp"
#include "../../core/Config.hpp"
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

    Mesher::Mesher(const MeshConfig& config) : m_config(config) {
        m_lastStats = {};
    }

    void Mesher::BuildSectionMesh(const Game::Chunk& chunk, int sectionY, SectionMesh& outMesh) {
        auto startTime = std::chrono::high_resolution_clock::now();

        // Reset stats
        m_lastStats = {};

        // Clear output mesh
        outMesh.Clear();
        outMesh.chunkPos = chunk.pos;
        outMesh.sectionY = sectionY;

        // Reserve space for estimated geometry
        outMesh.Reserve(1024); // Estimate ~1024 quads per section

        // Check if section exists
        const Game::ChunkSection* section = chunk.GetSection(sectionY);
        if (!section) {
            // Empty section, nothing to mesh
            return;
        }

        // Process all blocks in this 16x16x16 section
        for (int localX = 0; localX < 16; ++localX) {
            for (int localY = 0; localY < 16; ++localY) {
                for (int localZ = 0; localZ < 16; ++localZ) {
                    Game::BlockID blockId = section->GetBlockID(localX, localY, localZ);

                    if (blockId != Game::BlockID::Air) {
                        ProcessBlock(chunk, localX, localY, localZ, sectionY, outMesh);
                    }
                }
            }
        }

        // Calculate build time
        auto endTime = std::chrono::high_resolution_clock::now();
        m_lastStats.buildTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        Log::Debug("Built section mesh (%d, %d, %d): %zu total vertices, %.2f ms",
                  chunk.pos.x, sectionY, chunk.pos.z,
                  outMesh.GetTotalVertexCount(), m_lastStats.buildTimeMs);
    }

    void Mesher::BuildChunkMesh(const Game::Chunk& chunk, ChunkMesh& outMesh) {
        outMesh.Clear();
        outMesh.chunkPos = chunk.pos;

        auto startTime = std::chrono::high_resolution_clock::now();

        // Build each section
        for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
            BuildSectionMesh(chunk, sectionY, outMesh.GetSection(sectionY));
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        float totalTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        Log::Debug("Built chunk mesh (%d, %d): %zu total vertices, %.2f ms",
                  chunk.pos.x, chunk.pos.z,
                  outMesh.GetTotalVertexCount(), totalTime);
    }

    void Mesher::ProcessBlock(const Game::Chunk& chunk, int localX, int localY, int localZ,
                             int sectionY, SectionMesh& mesh) {

        // Calculate world Y coordinate
        int worldY = sectionY * 16 + localY + Config::MinY;

        // Get block ID
        const Game::ChunkSection* section = chunk.GetSection(sectionY);
        if (!section) return;

        Game::BlockID blockId = section->GetBlockID(localX, localY, localZ);
        if (blockId == Game::BlockID::Air) return;

        // Handle fluid blocks separately
        if (blockId == Game::BlockID::Water || blockId == Game::BlockID::Lava) {
            FluidMeshBuilder fluidBuilder;
            fluidBuilder.BuildFluidBlock(chunk, localX, worldY, localZ, sectionY, mesh);
            return;
        }

        // Get block model
        const Game::BlockModel& model = Game::BlockRegistry::GetBlockModel(blockId);

        // Calculate world position for this block
        glm::vec3 worldPos = LocalToWorldPos(chunk.pos, localX, worldY, localZ);

        // Process each element in the block model
        for (const auto& element : model.elements) {
            // Process each face of the element
            for (const auto& [faceDir, faceDef] : element.faces) {
                // Convert to our face enum
                BlockFace blockFace = static_cast<BlockFace>(static_cast<int>(faceDir));

                // Check if face should be culled
                if (m_config.enableFaceCulling) {
                    int chunkLocalY = worldY - Config::MinY;
                    if (ShouldCullFace(chunk, localX, chunkLocalY, localZ, blockFace, blockId)) {
                        m_lastStats.facesCulled++;
                        continue;
                    }
                }

                // Get face normal
                glm::vec3 faceNormal = GetFaceNormal(blockFace);

                // Add this face to the mesh
                AddBlockFace(model, element, faceDir, faceDef, worldPos, faceNormal, mesh);
                m_lastStats.facesGenerated++;
            }
        }
    }

    void Mesher::AddBlockFace(const Game::BlockModel& model, const Game::Element& element,
                             Game::FaceDir faceDir, const Game::FaceDef& faceDef,
                             glm::vec3 blockPos, glm::vec3 faceNormal, SectionMesh& mesh) {

        // Resolve texture path
        std::string texturePath = model.ResolveTexture(faceDef.textureRef);

        // Get UV coordinates from atlas
        glm::vec4 uvRect;
        if (!GetTextureUV(texturePath, uvRect)) {
            // Use error texture if not found
            texturePath = "missingno";
            GetTextureUV(texturePath, uvRect);
        }

        // Calculate biome tint if needed
        glm::vec4 tintColor(1.0f);
        if (m_config.enableBiomeTinting && faceDef.tintIndex >= 0) {
            // For now, use white tint - in full implementation would sample colormap
            tintColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        }

        // Convert face direction to our BlockFace enum
        BlockFace blockFace = static_cast<BlockFace>(static_cast<int>(faceDir));

        // Create face vertices
        std::vector<Vertex> faceVerts = CreateFaceVertices(blockPos, blockFace, uvRect, tintColor);

        // Determine which render layer this goes into
        Game::BlockID blockId = Game::BlockID::Stone; // Would get from context in real implementation
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

    bool Mesher::ShouldCullFace(const Game::Chunk& chunk, int x, int y, int z,
                               BlockFace face, Game::BlockID currentBlock) {
        // Get neighbor block
        Game::BlockID neighborBlock = GetNeighborBlock(chunk, x, y, z, face);

        // Don't cull if neighbor is air
        if (neighborBlock == Game::BlockID::Air) {
            return false;
        }

        // Check if neighbor is opaque and covers this face
        return IsBlockOpaque(neighborBlock);
    }

    Game::BlockID Mesher::GetNeighborBlock(const Game::Chunk& chunk, int x, int y, int z,
                                          BlockFace face) {
        glm::ivec3 offset = FACE_OFFSETS[static_cast<int>(face)];
        glm::ivec3 neighborPos = glm::ivec3(x, y, z) + offset;

        // Check if neighbor is within chunk bounds
        if (neighborPos.x >= 0 && neighborPos.x < 16 &&
            neighborPos.y >= 0 && neighborPos.y < Game::Math::CHUNK_TOTAL_HEIGHT &&
            neighborPos.z >= 0 && neighborPos.z < 16) {

            return chunk.GetBlock(neighborPos.x, neighborPos.y, neighborPos.z);
        }

        // Neighbor is outside chunk - would need cross-chunk access in full implementation
        return Game::BlockID::Air;
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

        // **FIXED**: Corrected top face winding order to face upward instead of downward
        switch (face) {
            case BlockFace::PositiveY: // Top face (+Y)
                // When viewed from above, vertices should go counter-clockwise
                vertices[0] = Vertex(blockPos + glm::vec3(0, 1, 1), normal, glm::vec2(uvRect.x, uvRect.y), tint); // Front-left
                vertices[1] = Vertex(blockPos + glm::vec3(1, 1, 1), normal, glm::vec2(uvRect.z, uvRect.y), tint); // Front-right
                vertices[2] = Vertex(blockPos + glm::vec3(1, 1, 0), normal, glm::vec2(uvRect.z, uvRect.w), tint); // Back-right
                vertices[3] = Vertex(blockPos + glm::vec3(0, 1, 0), normal, glm::vec2(uvRect.x, uvRect.w), tint); // Back-left
                break;

            case BlockFace::NegativeY: // Bottom face (-Y)
                vertices[0] = Vertex(blockPos + glm::vec3(0, 0, 0), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(1, 0, 0), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(1, 0, 1), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(0, 0, 1), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                break;

            case BlockFace::PositiveZ: // Front face (+Z)
                vertices[0] = Vertex(blockPos + glm::vec3(0, 0, 1), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(1, 0, 1), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(1, 1, 1), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(0, 1, 1), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                break;

            case BlockFace::NegativeZ: // Back face (-Z)
                vertices[0] = Vertex(blockPos + glm::vec3(1, 0, 0), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(0, 0, 0), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(0, 1, 0), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(1, 1, 0), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                break;

            case BlockFace::PositiveX: // Right face (+X)
                vertices[0] = Vertex(blockPos + glm::vec3(1, 0, 1), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(1, 0, 0), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(1, 1, 0), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(1, 1, 1), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                break;

            case BlockFace::NegativeX: // Left face (-X)
                vertices[0] = Vertex(blockPos + glm::vec3(0, 0, 0), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(0, 0, 1), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(0, 1, 1), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(0, 1, 0), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                break;
        }

        return vertices;
    }

    glm::vec3 Mesher::GetFaceNormal(BlockFace face) {
        return FACE_NORMALS[static_cast<int>(face)];
    }

    uint8_t Mesher::CalculateAmbientOcclusion(const Game::Chunk& chunk, int x, int y, int z,
                                             BlockFace face, int vertexIndex) {
        if (!m_config.enableAmbientOcclusion) {
            return 255; // Full brightness
        }

        // Simple AO implementation - would be more sophisticated in full version
        // For now, just return full brightness
        return 255;
    }

    int Mesher::WorldYToChunkY(int worldY) const {
        return worldY - Config::MinY;
    }

    glm::vec3 Mesher::LocalToWorldPos(const Game::Math::ChunkPos& chunkPos,
                                     int localX, int localY, int localZ) const {
        return glm::vec3(
            chunkPos.x * Game::Math::CHUNK_SIZE_X + localX,
            localY,
            chunkPos.z * Game::Math::CHUNK_SIZE_Z + localZ
        );
    }

    // Render layer classification functions
    RenderLayer ClassifyBlock(Game::BlockID blockId) {
        const Game::Block& block = Game::BlockRegistry::Get(blockId);

        // Check for transparent blocks first
        if (block.isTransparent) {
            // Distinguish between cutout and translucent
            switch (blockId) {
                case Game::BlockID::Leaves:
                case Game::BlockID::CherryLeaves:
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