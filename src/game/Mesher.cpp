// File: src/game/Mesher.cpp
#include "Mesher.hpp"
#include "BlockRegistry.hpp"
#include "EnhancedBlockRegistry.hpp"
#include "BlockModel.hpp"
#include "../render/AtlasBuilder.hpp"
#include "../core/Log.hpp"
#include "../core/Config.hpp"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace Game {

    void Mesher::MeshSection(ChunkSection* section, MeshData* meshData, Chunk* parentChunk) {
        if (!section || !meshData || !parentChunk) {
            Log::Warning("Invalid parameters passed to MeshSection");
            return;
        }

        // Call internal implementation without neighbor context
        MeshSectionInternal(section, meshData, parentChunk->pos, 0, nullptr);
    }

    void Mesher::MeshSectionWithNeighbors(ChunkSection* section, MeshData* meshData,
                                        const NeighborContext& context) {
        if (!section || !meshData || !context.center) {
            Log::Warning("Invalid parameters passed to MeshSectionWithNeighbors");
            return;
        }

        // Extract section index from meshData if available, otherwise assume 0
        int sectionIndex = meshData->sectionIndex;

        // Call internal implementation with neighbor context
        MeshSectionInternal(section, meshData, context.center->pos, sectionIndex, &context);
    }

    void Mesher::MeshSectionInternal(ChunkSection* section, MeshData* meshData,
                                   Math::ChunkPos chunkPos, int sectionIndex,
                                   const NeighborContext* neighborContext) {

        // Clear existing data
        meshData->vertices.clear();
        meshData->indices.clear();
        meshData->chunkXZ = chunkPos;
        meshData->sectionIndex = sectionIndex;

        // Calculate world Y offset for this section
        int worldYOffset = Config::MinY + (sectionIndex * Math::SECTION_HEIGHT);

        // Iterate through all blocks in the section
        for (int x = 0; x < ChunkSection::SIZE; ++x) {
            for (int z = 0; z < ChunkSection::SIZE; ++z) {
                for (int y = 0; y < ChunkSection::SIZE; ++y) {
                    BlockID blockId = section->GetBlockID(x, y, z);

                    // Skip air blocks
                    if (blockId == BlockID::Air) {
                        continue;
                    }

                    // Get enhanced block definition
                    const EnhancedBlock& enhancedBlock = EnhancedBlockRegistry::Get(blockId);

                    // Handle legacy blocks differently
                    if (enhancedBlock.useLegacyTextures) {
                        // TODO: Implement legacy block meshing if needed
                        continue;
                    }

                    // Get the block model
                    const BlockModel& model = EnhancedBlockRegistry::GetBlockModel(blockId);

                    // Calculate positions
                    glm::ivec3 blockPos(x, y, z);  // Local position within section
                    glm::ivec3 worldBlockPos(
                        chunkPos.x * Math::CHUNK_SIZE_X + x,
                        worldYOffset + y,
                        chunkPos.z * Math::CHUNK_SIZE_Z + z
                    );

                    // Mesh all elements of this block model
                    MeshElement(model.elements[0], model, blockPos, worldBlockPos,
                              meshData, enhancedBlock.enableBiomeTinting,
                              neighborContext, neighborContext ? neighborContext->center.get() : nullptr);

                    // Mesh additional elements if they exist
                    for (size_t i = 1; i < model.elements.size(); ++i) {
                        MeshElement(model.elements[i], model, blockPos, worldBlockPos,
                                  meshData, enhancedBlock.enableBiomeTinting,
                                  neighborContext, neighborContext ? neighborContext->center.get() : nullptr);
                    }
                }
            }
        }

        Log::Debug("Meshed section (%d, %d, %d) with %zu vertices, %zu indices",
                  chunkPos.x, chunkPos.z, sectionIndex,
                  meshData->vertices.size(), meshData->indices.size());
    }

    void Mesher::MeshElement(const Element& element, const BlockModel& model,
                           const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                           MeshData* meshData, bool enableBiomeTinting,
                           const NeighborContext* neighborContext, Chunk* chunk) {

        // Iterate through all faces of this element
        for (const auto& [faceDir, faceDef] : element.faces) {
            // Check if this face should be culled
            FaceDirection mesherFace = ModelFaceToMesherFace(faceDir);

            int nx, ny, nz;
            GetNeighborPos(blockPos.x, worldBlockPos.y, blockPos.z, mesherFace, nx, ny, nz);

            BlockID neighborBlock = BlockID::Air;

            // Get neighbor block using appropriate method
            if (neighborContext && neighborContext->hasAllNeighbors) {
                neighborBlock = GetBlockWithNeighbors(*neighborContext,
                    nx - (worldBlockPos.x - blockPos.x), ny, nz - (worldBlockPos.z - blockPos.z));
            } else if (chunk) {
                neighborBlock = GetBlockStandard(chunk,
                    nx - (worldBlockPos.x - blockPos.x), ny, nz - (worldBlockPos.z - blockPos.z));
            }

            // Get current block ID from enhanced registry
            // We need to determine current block ID from context
            BlockID currentBlock = BlockID::Stone; // Placeholder - should be determined from context

            // Cull face if neighbor is opaque
            if (ShouldCullFace(currentBlock, neighborBlock)) {
                continue;
            }

            // Mesh this face
            MeshFace(element, faceDef, faceDir, model, blockPos, worldBlockPos,
                    meshData, enableBiomeTinting);
        }
    }

    void Mesher::MeshFace(const Element& element, const FaceDef& faceDef,
                        FaceDir faceDir, const BlockModel& model,
                        const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                        MeshData* meshData, bool enableBiomeTinting) {

        // Get face geometry
        glm::vec3 normal = GetFaceNormal(faceDir);
        auto vertices = GetFaceVertices(element, faceDir);
        auto uvs = GetFaceUVs(faceDef);

        // Resolve texture path
        std::string texturePath = model.ResolveTexture(faceDef.textureRef);

        // Get biome tinting if enabled
        glm::vec3 tint(1.0f);
        if (enableBiomeTinting && faceDef.tintIndex >= 0) {
            tint = SampleBiomeTinting(faceDef.tintIndex, worldBlockPos);
        }

        // Create vertices
        uint32_t baseIndex = static_cast<uint32_t>(meshData->vertices.size());

        for (int i = 0; i < 4; ++i) {
            Render::Vertex vertex;

            // Position: convert from model space to world space
            vertex.pos = ModelToWorldSpace(vertices[i], blockPos, worldBlockPos);

            // Normal
            vertex.nrm = normal;

            // UV coordinates - convert model UV to atlas UV
            glm::vec2 atlasUV;
            if (!GetAtlasUVs(texturePath, uvs[i], atlasUV)) {
                // Fallback to default UV if atlas lookup fails
                atlasUV = uvs[i];
            }
            vertex.uv = atlasUV;

            // Ambient occlusion (placeholder)
            vertex.ao = 255;

            meshData->vertices.push_back(vertex);
        }

        // Create indices for two triangles (quad)
        // Triangle 1: 0, 1, 2
        meshData->indices.push_back(baseIndex + 0);
        meshData->indices.push_back(baseIndex + 1);
        meshData->indices.push_back(baseIndex + 2);

        // Triangle 2: 0, 2, 3
        meshData->indices.push_back(baseIndex + 0);
        meshData->indices.push_back(baseIndex + 2);
        meshData->indices.push_back(baseIndex + 3);
    }

    BlockID Mesher::GetBlockWithNeighbors(const NeighborContext& context,
                                        int localX, int worldY, int localZ) {
        // Handle within-chunk coordinates
        if (localX >= 0 && localX < Math::CHUNK_SIZE_X &&
            localZ >= 0 && localZ < Math::CHUNK_SIZE_Z) {
            return context.center->GetBlock(localX, worldY, localZ);
        }

        // Handle cross-chunk coordinates
        std::shared_ptr<Chunk> targetChunk = nullptr;
        int targetX = localX;
        int targetZ = localZ;

        if (localX < 0) {
            targetChunk = context.neighbors[0]; // West neighbor
            targetX = localX + Math::CHUNK_SIZE_X;
        } else if (localX >= Math::CHUNK_SIZE_X) {
            targetChunk = context.neighbors[1]; // East neighbor
            targetX = localX - Math::CHUNK_SIZE_X;
        } else if (localZ < 0) {
            targetChunk = context.neighbors[2]; // North neighbor
            targetZ = localZ + Math::CHUNK_SIZE_Z;
        } else if (localZ >= Math::CHUNK_SIZE_Z) {
            targetChunk = context.neighbors[3]; // South neighbor
            targetZ = localZ - Math::CHUNK_SIZE_Z;
        }

        if (!targetChunk) {
            return BlockID::Air; // Neighbor not available
        }

        return targetChunk->GetBlock(targetX, worldY, targetZ);
    }

    BlockID Mesher::GetBlockStandard(Chunk* chunk, int localX, int worldY, int localZ) {
        // Bounds check
        if (localX < 0 || localX >= Math::CHUNK_SIZE_X ||
            localZ < 0 || localZ >= Math::CHUNK_SIZE_Z ||
            worldY < Config::MinY || worldY > Config::MaxY) {
            return BlockID::Air;
        }

        return chunk->GetBlock(localX, worldY, localZ);
    }

    bool Mesher::ShouldCullFace(BlockID currentBlock, BlockID neighborBlock) {
        if (neighborBlock == BlockID::Air) {
            return false; // Don't cull faces adjacent to air
        }

        // Get neighbor block properties
        const EnhancedBlock& neighborEnhanced = EnhancedBlockRegistry::Get(neighborBlock);

        // Don't cull if neighbor is transparent
        if (neighborEnhanced.isTransparent) {
            return false;
        }

        // Cull if neighbor is opaque
        return neighborEnhanced.opaque;
    }

    void Mesher::GetNeighborPos(int x, int y, int z, FaceDirection faceDir,
                              int& nx, int& ny, int& nz) {
        nx = x;
        ny = y;
        nz = z;

        switch (faceDir) {
            case FaceDirection::PosX: nx++; break; // East
            case FaceDirection::NegX: nx--; break; // West
            case FaceDirection::PosY: ny++; break; // Up
            case FaceDirection::NegY: ny--; break; // Down
            case FaceDirection::PosZ: nz++; break; // South
            case FaceDirection::NegZ: nz--; break; // North
        }
    }

    FaceDirection Mesher::ModelFaceToMesherFace(FaceDir modelFace) {
        switch (modelFace) {
            case FaceDir::East:  return FaceDirection::PosX;
            case FaceDir::West:  return FaceDirection::NegX;
            case FaceDir::Up:    return FaceDirection::PosY;
            case FaceDir::Down:  return FaceDirection::NegY;
            case FaceDir::South: return FaceDirection::PosZ;
            case FaceDir::North: return FaceDirection::NegZ;
            default: return FaceDirection::PosY;
        }
    }

    glm::vec3 Mesher::GetFaceNormal(FaceDir faceDir) {
        switch (faceDir) {
            case FaceDir::Up:    return glm::vec3(0.0f, 1.0f, 0.0f);
            case FaceDir::Down:  return glm::vec3(0.0f, -1.0f, 0.0f);
            case FaceDir::North: return glm::vec3(0.0f, 0.0f, -1.0f);
            case FaceDir::South: return glm::vec3(0.0f, 0.0f, 1.0f);
            case FaceDir::West:  return glm::vec3(-1.0f, 0.0f, 0.0f);
            case FaceDir::East:  return glm::vec3(1.0f, 0.0f, 0.0f);
            default: return glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }

    std::array<glm::vec3, 4> Mesher::GetFaceVertices(const Element& element, FaceDir faceDir) {
        std::array<glm::vec3, 4> vertices;

        // Get element bounds in model space (0-16)
        glm::vec3 min = element.from;
        glm::vec3 max = element.to;

        switch (faceDir) {
            case FaceDir::Up: // +Y face
                vertices[0] = glm::vec3(min.x, max.y, min.z); // Bottom-left
                vertices[1] = glm::vec3(max.x, max.y, min.z); // Bottom-right
                vertices[2] = glm::vec3(max.x, max.y, max.z); // Top-right
                vertices[3] = glm::vec3(min.x, max.y, max.z); // Top-left
                break;

            case FaceDir::Down: // -Y face
                vertices[0] = glm::vec3(min.x, min.y, max.z); // Bottom-left
                vertices[1] = glm::vec3(max.x, min.y, max.z); // Bottom-right
                vertices[2] = glm::vec3(max.x, min.y, min.z); // Top-right
                vertices[3] = glm::vec3(min.x, min.y, min.z); // Top-left
                break;

            case FaceDir::North: // -Z face
                vertices[0] = glm::vec3(max.x, min.y, min.z); // Bottom-left
                vertices[1] = glm::vec3(min.x, min.y, min.z); // Bottom-right
                vertices[2] = glm::vec3(min.x, max.y, min.z); // Top-right
                vertices[3] = glm::vec3(max.x, max.y, min.z); // Top-left
                break;

            case FaceDir::South: // +Z face
                vertices[0] = glm::vec3(min.x, min.y, max.z); // Bottom-left
                vertices[1] = glm::vec3(max.x, min.y, max.z); // Bottom-right
                vertices[2] = glm::vec3(max.x, max.y, max.z); // Top-right
                vertices[3] = glm::vec3(min.x, max.y, max.z); // Top-left
                break;

            case FaceDir::West: // -X face
                vertices[0] = glm::vec3(min.x, min.y, min.z); // Bottom-left
                vertices[1] = glm::vec3(min.x, min.y, max.z); // Bottom-right
                vertices[2] = glm::vec3(min.x, max.y, max.z); // Top-right
                vertices[3] = glm::vec3(min.x, max.y, min.z); // Top-left
                break;

            case FaceDir::East: // +X face
                vertices[0] = glm::vec3(max.x, min.y, max.z); // Bottom-left
                vertices[1] = glm::vec3(max.x, min.y, min.z); // Bottom-right
                vertices[2] = glm::vec3(max.x, max.y, min.z); // Top-right
                vertices[3] = glm::vec3(max.x, max.y, max.z); // Top-left
                break;
        }

        return vertices;
    }

    std::array<glm::vec2, 4> Mesher::GetFaceUVs(const FaceDef& faceDef) {
        // faceDef.uv contains [u1, v1, u2, v2] in texture pixel coordinates
        // We need to normalize these and create corner UVs

        float u1 = faceDef.uv.x / 16.0f; // Assuming 16x16 texture
        float v1 = faceDef.uv.y / 16.0f;
        float u2 = faceDef.uv.z / 16.0f;
        float v2 = faceDef.uv.w / 16.0f;

        return {
            glm::vec2(u1, v2), // Bottom-left
            glm::vec2(u2, v2), // Bottom-right
            glm::vec2(u2, v1), // Top-right
            glm::vec2(u1, v1)  // Top-left
        };
    }

    glm::vec3 Mesher::ModelToWorldSpace(const glm::vec3& modelPos,
                                      const glm::ivec3& blockPos,
                                      const glm::ivec3& worldBlockPos) {
        // Convert from model space (0-16) to world space
        glm::vec3 normalizedPos = modelPos / 16.0f; // Normalize to 0-1
        return glm::vec3(worldBlockPos) + normalizedPos;
    }

    glm::vec3 Mesher::SampleBiomeTinting(int tintIndex, const glm::ivec3& worldPos) {
        // Placeholder biome tinting - sample at a fixed coordinate for now
        // In a real implementation, this would sample based on the actual biome

        if (tintIndex == 0) {
            // Grass tinting - return a green tint with some variation
            float variation = sin(worldPos.x * 0.1f) * cos(worldPos.z * 0.1f) * 0.1f;
            return glm::vec3(0.4f + variation, 0.8f + variation, 0.4f + variation);
        } else {
            // Foliage tinting - return a slightly different green
            float variation = cos(worldPos.x * 0.15f) * sin(worldPos.z * 0.15f) * 0.1f;
            return glm::vec3(0.3f + variation, 0.7f + variation, 0.3f + variation);
        }
    }

    bool Mesher::GetAtlasUVs(const std::string& texturePath,
                           const glm::vec2& modelUV, glm::vec2& atlasUV) {
        // Try to get UVs from AtlasBuilder first
        if (Render::g_atlasBuilder) {
            Render::AtlasUVRect uvRect;
            if (Render::g_atlasBuilder->GetUVRect(texturePath, uvRect)) {
                // Interpolate within the atlas rect using model UV
                atlasUV = glm::mix(uvRect.uvMin, uvRect.uvMax, modelUV);
                return true;
            }
        }

        // Fallback to legacy atlas system
        // This would need to be implemented based on your legacy system
        atlasUV = modelUV; // Simple fallback
        return false;
    }

    // Legacy entry points
    void Mesher::MesherJob(ChunkSection* section, MeshData* meshData, Chunk* parentChunk) {
        MeshSection(section, meshData, parentChunk);
    }

    void Mesher::InterChunkMesherJob(ChunkSection* section, MeshData* meshData,
                                   const NeighborContext& context) {
        MeshSectionWithNeighbors(section, meshData, context);
    }

    // Global convenience functions
    void MesherJob(ChunkSection* section, MeshData* meshData, Chunk* parentChunk) {
        Mesher::MesherJob(section, meshData, parentChunk);
    }

    void InterChunkMesherJob(ChunkSection* section, MeshData* meshData,
                           const NeighborContext& context) {
        Mesher::InterChunkMesherJob(section, meshData, context);
    }

} // namespace Game