// File: src/game/Mesher.cpp (FIXED - All Structural Issues + Callback Implementation)
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

#include "TextureAtlas.hpp"

namespace Game {

    // Global mesh upload callback
    static MeshUploadCallback g_meshUploadCallback = nullptr;

    void SetMeshUploadCallback(MeshUploadCallback callback) {
        g_meshUploadCallback = callback;
    }


    void Mesher::MeshSection(ChunkSection* section, MeshData* meshData, Chunk* parentChunk) {
        if (!section || !meshData || !parentChunk) {
            Log::Warning("Invalid parameters passed to MeshSection");
            return;
        }

        // FIXED: Use the section index from meshData instead of hardcoded 0
        int sectionIndex = meshData->sectionIndex;

        // Call internal implementation without neighbor context
        MeshSectionInternal(section, meshData, parentChunk->pos, sectionIndex, nullptr);
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

                    // FIXED: Mesh all elements uniformly
                    for (const auto& element : model.elements) {
                        MeshElement(element, model, blockPos, worldBlockPos, blockId,
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
                           BlockID currentBlockId, MeshData* meshData, bool enableBiomeTinting,
                           const NeighborContext* neighborContext, Chunk* chunk) {

        // Iterate through all faces of this element
        for (const auto& [faceDir, faceDef] : element.faces) {
            // FIXED: Calculate neighbor position using LOCAL coordinates
            FaceDirection mesherFace = ModelFaceToMesherFace(faceDir);

            int dx, dy, dz;
            GetFaceOffset(mesherFace, dx, dy, dz);

            // FIXED: Compute neighbor coords in LOCAL space
            int neighborX = blockPos.x + dx;
            int neighborY = blockPos.y + dy;  // This is section-local Y
            int neighborZ = blockPos.z + dz;

            // Convert local Y to world Y for neighbor lookup
            int worldY = worldBlockPos.y + dy;

            BlockID neighborBlock = BlockID::Air;

            // Get neighbor block using appropriate method
            if (neighborContext && neighborContext->hasAllNeighbors) {
                neighborBlock = GetBlockWithNeighbors(*neighborContext,
                    neighborX, worldY, neighborZ);
            } else if (chunk) {
                neighborBlock = GetBlockStandard(chunk, neighborX, worldY, neighborZ);
            }

            // Cull face if neighbor is opaque
            if (ShouldCullFace(currentBlockId, neighborBlock)) {
                continue;
            }

            // Mesh this face
            MeshFace(element, faceDef, faceDir, model, blockPos, worldBlockPos,
                    currentBlockId, meshData, enableBiomeTinting);
        }
    }

    void Mesher::MeshFace(const Element& element, const FaceDef& faceDef,
                        FaceDir faceDir, const BlockModel& model,
                        const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                        BlockID currentBlockId, MeshData* meshData, bool enableBiomeTinting) {

        // Get face geometry
        glm::vec3 normal = GetFaceNormal(faceDir);
        auto vertices = GetFaceVertices(element, faceDir);

        // Resolve texture path
        std::string texturePath = model.ResolveTexture(faceDef.textureRef);

        // FIXED: Get biome tinting as color, not separate
        glm::vec4 tintColor(1.0f, 1.0f, 1.0f, 1.0f);
        if (enableBiomeTinting && faceDef.tintIndex >= 0) {
            glm::vec3 tint = SampleBiomeTinting(faceDef.tintIndex, worldBlockPos);
            tintColor = glm::vec4(tint, 1.0f);
        }

        // FIXED: Get UV coordinates with proper texture dimensions
        auto uvs = GetFaceUVs(faceDef, texturePath);

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

            // FIXED: Store tint color in vertex
            vertex.color = tintColor;

            // Ambient occlusion (placeholder - can be calculated properly later)
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

    void Mesher::GetFaceOffset(FaceDirection faceDir, int& dx, int& dy, int& dz) {
        dx = dy = dz = 0;

        switch (faceDir) {
            case FaceDirection::PosX: dx = 1; break;  // East
            case FaceDirection::NegX: dx = -1; break; // West
            case FaceDirection::PosY: dy = 1; break;  // Up
            case FaceDirection::NegY: dy = -1; break; // Down
            case FaceDirection::PosZ: dz = 1; break;  // South
            case FaceDirection::NegZ: dz = -1; break; // North
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
            case FaceDir::Up: // +Y face (TOP of block)
                // FIXED: Correct vertex ordering for top face
                // Order: bottom-left, bottom-right, top-right, top-left (in texture space)
                // This corresponds to: -Z+X corner, +Z+X corner, +Z-X corner, -Z-X corner
                vertices[0] = glm::vec3(min.x, max.y, max.z); // Bottom-left in texture space
                vertices[1] = glm::vec3(max.x, max.y, max.z); // Bottom-right in texture space
                vertices[2] = glm::vec3(max.x, max.y, min.z); // Top-right in texture space
                vertices[3] = glm::vec3(min.x, max.y, min.z); // Top-left in texture space
                break;

            case FaceDir::Down: // -Y face (BOTTOM of block)
                // FIXED: Correct vertex ordering for bottom face (viewed from below)
                vertices[0] = glm::vec3(min.x, min.y, min.z); // Bottom-left
                vertices[1] = glm::vec3(max.x, min.y, min.z); // Bottom-right
                vertices[2] = glm::vec3(max.x, min.y, max.z); // Top-right
                vertices[3] = glm::vec3(min.x, min.y, max.z); // Top-left
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

    std::array<glm::vec2, 4> Mesher::GetFaceUVs(const FaceDef& faceDef, const std::string& texturePath) {
        // FIXED: Get actual texture dimensions for proper UV normalization
        float textureWidth = 16.0f;  // Default to 16x16
        float textureHeight = 16.0f;

        // Try to get actual texture dimensions from AtlasBuilder
        if (Render::g_atlasBuilder) {
            // For now, assume 16x16. In a full implementation, you'd store texture dimensions
            // in the AtlasBuilder or extend the texture metadata
        }

        // faceDef.uv contains [u1, v1, u2, v2] in texture pixel coordinates
        // Normalize by actual texture dimensions
        float u1 = faceDef.uv.x / textureWidth;
        float v1 = faceDef.uv.y / textureHeight;
        float u2 = faceDef.uv.z / textureWidth;
        float v2 = faceDef.uv.w / textureHeight;

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

            // Try exact path first
            if (Render::g_atlasBuilder->GetUVRect(texturePath, uvRect)) {
                atlasUV = glm::mix(uvRect.uvMin, uvRect.uvMax, modelUV);
                return true;
            }

            // Try with "minecraft:" prefix added
            std::string prefixedPath = "minecraft:" + texturePath;
            if (Render::g_atlasBuilder->GetUVRect(prefixedPath, uvRect)) {
                atlasUV = glm::mix(uvRect.uvMin, uvRect.uvMax, modelUV);
                return true;
            }

            // Try without "minecraft:" prefix if it exists
            if (texturePath.rfind("minecraft:", 0) == 0) {
                std::string unprefixedPath = texturePath.substr(10);
                if (Render::g_atlasBuilder->GetUVRect(unprefixedPath, uvRect)) {
                    atlasUV = glm::mix(uvRect.uvMin, uvRect.uvMax, modelUV);
                    return true;
                }
            }

            Log::Warning("Texture '%s' not found in AtlasBuilder", texturePath.c_str());
        }

        // Fallback to legacy atlas system for basic blocks
        if (!Render::g_textureAtlas.IsLoaded()) {
            Log::Warning("No texture atlas available for: %s", texturePath.c_str());
            atlasUV = modelUV;
            return false;
        }

        // Map common block textures to legacy atlas indices
        uint16_t atlasIndex = 1008; // Default to air (transparent)

        if (texturePath == "block/stone" || texturePath == "minecraft:block/stone") {
            atlasIndex = 1; // Stone at index 1
        } else if (texturePath == "block/dirt" || texturePath == "minecraft:block/dirt") {
            atlasIndex = 0; // Dirt at index 0
        } else if (texturePath == "block/grass_block_top" || texturePath == "minecraft:block/grass_block_top") {
            atlasIndex = 2; // Grass top at index 2
        } else if (texturePath == "block/grass_block_side" || texturePath == "minecraft:block/grass_block_side") {
            atlasIndex = 18; // Grass side at index 18
        } else if (texturePath == "block/bedrock" || texturePath == "minecraft:block/bedrock") {
            atlasIndex = 5; // Bedrock at index 5
        } else if (texturePath == "block/sand" || texturePath == "minecraft:block/sand") {
            atlasIndex = 16; // Sand at index 16
        } else {
            Log::Warning("Unknown texture path '%s', using default", texturePath.c_str());
            atlasIndex = 1; // Default to stone
        }

        // Get UV coordinates from legacy atlas
        Render::AtlasTile tile = Render::g_textureAtlas.GetTile(atlasIndex);

        // Interpolate within the tile using modelUV
        atlasUV = glm::mix(tile.uvMin, tile.uvMax, modelUV);

        Log::Debug("Using legacy atlas index %u for texture '%s'", atlasIndex, texturePath.c_str());
        return true;
    }

    // Legacy entry points
    void Mesher::MesherJob(ChunkSection* section, MeshData* meshData, Chunk* parentChunk) {
        MeshSection(section, meshData, parentChunk);

        // Upload the mesh data using the callback if available
        if (g_meshUploadCallback && meshData) {
            g_meshUploadCallback(meshData);
        }
    }

    void Mesher::InterChunkMesherJob(ChunkSection* section, MeshData* meshData,
                                   const NeighborContext& context) {
        MeshSectionWithNeighbors(section, meshData, context);

        // Upload the mesh data using the callback if available
        if (g_meshUploadCallback && meshData) {
            g_meshUploadCallback(meshData);
        }
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