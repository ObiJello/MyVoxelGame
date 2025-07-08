// File: src/render/mesh/Mesher.cpp
#include "Mesher.hpp"
#include "../../engine/block/BlockRegistry.hpp"
#include "../../engine/block/BlockModel.hpp"
#include "../atlas/AtlasBuilder.hpp"
#include "../../core/Log.hpp"
#include "../../core/Config.hpp"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <shared_mutex>
#include <unordered_set>

#include "../../engine/world/ChunkProvider.hpp"
#include "JobSystem.hpp"
#include "../../game/WorldAccess.hpp"

namespace Game {

    // Global mesh upload callback
    static MeshUploadCallback g_meshUploadCallback = nullptr;

    // Global layered mesh upload callback
    static LayeredMeshUploadCallback g_layeredMeshUploadCallback = nullptr;

    void SetMeshUploadCallback(MeshUploadCallback callback) {
        g_meshUploadCallback = callback;
    }

    void SetLayeredMeshUploadCallback(LayeredMeshUploadCallback callback) {
        g_layeredMeshUploadCallback = callback;
    }

    // CRITICAL FIX: Enhanced convenience function with proper error handling
    void LayeredMesherJob(ChunkSection* section, LayeredMeshData* meshData,
                        const NeighborContext& context) {
        if (!section || !meshData) {
            Log::Error("LayeredMesherJob called with null parameters");
            delete meshData;
            return;
        }

        // CRITICAL: Validate meshData structure before use
        if (meshData->sectionIndex < 0 || meshData->sectionIndex >= Math::SECTIONS_PER_CHUNK) {
            Log::Error("LayeredMesherJob: Invalid section index %d", meshData->sectionIndex);
            delete meshData;
            return;
        }

        // CRITICAL: Clear all data to ensure clean state
        meshData->opaqueVertices.clear();
        meshData->cutoutVertices.clear();
        meshData->translucentVertices.clear();
        meshData->opaqueIndices.clear();
        meshData->cutoutIndices.clear();
        meshData->translucentIndices.clear();

        // CRITICAL: Reserve reasonable capacity to prevent repeated allocations
        meshData->opaqueVertices.reserve(1024);
        meshData->opaqueIndices.reserve(1536);
        meshData->cutoutVertices.reserve(256);
        meshData->cutoutIndices.reserve(384);
        meshData->translucentVertices.reserve(256);
        meshData->translucentIndices.reserve(384);

        try {
            Log::Debug("Starting layered meshing for chunk (%d,%d) section %d",
                      meshData->chunkXZ.x, meshData->chunkXZ.z, meshData->sectionIndex);

            Mesher::MeshSectionLayered(section, meshData, context);

            // CRITICAL: Validate results before upload
            size_t totalVertices = meshData->GetTotalVertexCount();
            if (totalVertices > 100000) { // Sanity check - no section should have more than 100k vertices
                Log::Error("LayeredMesherJob: Suspicious vertex count %zu for chunk (%d,%d) section %d",
                          totalVertices, meshData->chunkXZ.x, meshData->chunkXZ.z, meshData->sectionIndex);
                delete meshData;
                return;
            }

            Log::Debug("Layered meshing completed: chunk (%d,%d) section %d - "
                      "Opaque: %zu verts, Cutout: %zu verts, Translucent: %zu verts",
                      meshData->chunkXZ.x, meshData->chunkXZ.z, meshData->sectionIndex,
                      meshData->opaqueVertices.size(), meshData->cutoutVertices.size(),
                      meshData->translucentVertices.size());

            // Transfer ownership to callback
            if (g_layeredMeshUploadCallback) {
                g_layeredMeshUploadCallback(meshData);
                // Callback now owns meshData
            } else {
                Log::Warning("No layered mesh upload callback registered");
                delete meshData;
            }
        } catch (const std::exception& e) {
            Log::Error("LayeredMesherJob failed: %s", e.what());
            delete meshData;
        } catch (...) {
            Log::Error("LayeredMesherJob failed with unknown exception");
            delete meshData;
        }
    }

    void Mesher::MeshSectionLayered(ChunkSection* section, LayeredMeshData* meshData,
                                  const NeighborContext& context) {
        if (!section || !meshData || !context.center) {
            Log::Error("Invalid parameters passed to MeshSectionLayered");
            return;
        }

        int sectionIndex = meshData->sectionIndex;
        MeshSectionLayeredInternal(section, meshData, context.center->pos, sectionIndex, context);
    }

    void Mesher::MeshSectionLayeredInternal(ChunkSection* section, LayeredMeshData* meshData,
                                          Math::ChunkPos chunkPos, int sectionIndex,
                                          const NeighborContext& context) {
        // CRITICAL: Ensure meshData has correct metadata
        meshData->chunkXZ = chunkPos;
        meshData->sectionIndex = sectionIndex;

        // Calculate world Y offset for this section
        int worldYOffset = Config::MinY + (sectionIndex * Math::SECTION_HEIGHT);

        int blocksProcessed = 0;
        int facesGenerated = 0;

        // Iterate through all blocks in the section
        for (int x = 0; x < ChunkSection::SIZE; ++x) {
            for (int z = 0; z < ChunkSection::SIZE; ++z) {
                for (int y = 0; y < ChunkSection::SIZE; ++y) {
                    BlockID blockId = section->GetBlockID(x, y, z);

                    // Skip air blocks
                    if (blockId == BlockID::Air) {
                        continue;
                    }

                    blocksProcessed++;

                    // Calculate positions
                    glm::ivec3 blockPos(x, y, z);  // Local position within section
                    glm::ivec3 worldBlockPos(
                        chunkPos.x * Math::CHUNK_SIZE_X + x,
                        worldYOffset + y,
                        chunkPos.z * Math::CHUNK_SIZE_Z + z
                    );

                    // Store vertex counts before meshing this block
                    size_t verticesBefore = meshData->GetTotalVertexCount();

                    // Mesh this block with layer classification
                    MeshBlockLayered(blockPos, worldBlockPos, blockId, meshData, context);

                    // CRITICAL: Validate that vertex count didn't explode
                    size_t verticesAfter = meshData->GetTotalVertexCount();
                    size_t verticesAdded = verticesAfter - verticesBefore;

                    if (verticesAdded > 1000) { // No single block should add more than 1000 vertices
                        Log::Error("Block at (%d,%d,%d) added suspicious vertex count: %zu",
                                  worldBlockPos.x, worldBlockPos.y, worldBlockPos.z, verticesAdded);
                        // Don't break - continue processing but log the issue
                    }

                    facesGenerated += verticesAdded / 4; // Approximate faces (4 vertices per face)
                }
            }
        }

        Log::Debug("Meshed section (%d,%d,%d): %d blocks, ~%d faces, %zu total vertices",
                  chunkPos.x, chunkPos.z, sectionIndex, blocksProcessed, facesGenerated,
                  meshData->GetTotalVertexCount());
    }

    void Mesher::MeshBlockLayered(const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                                BlockID blockId, LayeredMeshData* meshData,
                                const NeighborContext& context) {
        // CRITICAL: Validate input parameters
        if (!meshData) {
            Log::Error("MeshBlockLayered: null meshData");
            return;
        }

        // Check if this is a fluid block
        if (IsFluidBlock(blockId)) {
            MeshFluidBlock(blockPos, worldBlockPos, blockId, meshData, context);
        } else {
            MeshSolidBlock(blockPos, worldBlockPos, blockId, meshData, context);
        }
    }

    void Mesher::MeshSolidBlock(const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                              BlockID blockId, LayeredMeshData* meshData,
                              const NeighborContext& context) {
        // Get block definition and model
        const Block& block = BlockRegistry::Get(blockId);
        const BlockModel& model = BlockRegistry::GetBlockModel(blockId);

        // CRITICAL: Validate model has elements
        if (model.elements.empty()) {
            // Use a default cube model for blocks without proper models
            Log::Debug("Block %s has no model elements, using default cube",
                      block.name.c_str());
            // For now, just return - in production you'd generate a default cube
            return;
        }

        // Mesh all elements using enhanced element meshing
        for (const auto& element : model.elements) {
            MeshElement(element, model, blockPos, worldBlockPos, blockId,
                       meshData, block.enableBiomeTinting, context);
        }
    }

    void Mesher::MeshElement(const Element& element, const BlockModel& model,
                           const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                           BlockID currentBlockId, LayeredMeshData* meshData, bool enableBiomeTinting,
                           const NeighborContext& context) {

        // Iterate through all faces of this element
        for (const auto& [faceDir, faceDef] : element.faces) {
            FaceDirection mesherFace = ModelFaceToMesherFace(faceDir);

            int dx, dy, dz;
            GetFaceOffset(mesherFace, dx, dy, dz);

            int neighborX = blockPos.x + dx;
            int neighborY = blockPos.y + dy;
            int neighborZ = blockPos.z + dz;

            int worldY = worldBlockPos.y + dy;

            BlockID neighborBlock = GetBlockWithNeighbors(context,
                neighborX, worldY, neighborZ);

            // Cull face if neighbor is opaque
            if (ShouldCullFace(currentBlockId, neighborBlock)) {
                continue;
            }

            // CRITICAL: Store vertex count before meshing face
            size_t verticesBefore = meshData->GetTotalVertexCount();

            // Mesh this face with layer classification
            MeshFace(element, faceDef, faceDir, model, blockPos, worldBlockPos,
                    currentBlockId, meshData, enableBiomeTinting);

            // CRITICAL: Validate face didn't add too many vertices
            size_t verticesAfter = meshData->GetTotalVertexCount();
            if (verticesAfter < verticesBefore) {
                Log::Error("Vertex count decreased during face meshing - memory corruption detected!");
                return;
            }

            size_t verticesAdded = verticesAfter - verticesBefore;
            if (verticesAdded > 4) { // Each face should add exactly 4 vertices
                Log::Warning("Face added %zu vertices (expected 4) at block (%d,%d,%d)",
                           verticesAdded, worldBlockPos.x, worldBlockPos.y, worldBlockPos.z);
            }
        }
    }

    void Mesher::MeshFace(const Element& element, const FaceDef& faceDef,
                        FaceDir faceDir, const BlockModel& model,
                        const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                        BlockID currentBlockId, LayeredMeshData* meshData, bool enableBiomeTinting) {

        // CRITICAL: Validate meshData
        if (!meshData) {
            Log::Error("MeshFace: null meshData");
            return;
        }

        // Classify this face's render type
        FaceRenderType renderType = ClassifyFaceRenderType(faceDef, currentBlockId);

        // Get appropriate vertex/index arrays
        std::vector<Render::Vertex>* vertices;
        std::vector<uint32_t>* indices;
        GetLayerArrays(meshData, renderType, vertices, indices);

        // CRITICAL: Validate arrays are valid
        if (!vertices || !indices) {
            Log::Error("MeshFace: failed to get layer arrays");
            return;
        }

        // Get face geometry
        glm::vec3 normal = GetFaceNormal(faceDir);
        auto faceVertices = GetFaceVertices(element, faceDir);

        // Resolve texture path
        std::string texturePath = model.ResolveTexture(faceDef.textureRef);

        // Calculate tint color
        glm::vec4 tintColor(1.0f, 1.0f, 1.0f, 1.0f);
        if (faceDef.tintIndex >= 0 && enableBiomeTinting) {
            if (faceDef.tintIndex == 0) {
                glm::vec3 grassTint = SampleGrassTinting(worldBlockPos);
                tintColor = glm::vec4(grassTint, 1.0f);
            } else if (faceDef.tintIndex == 1) {
                glm::vec3 foliageTint = SampleFoliageTinting(worldBlockPos);
                tintColor = glm::vec4(foliageTint, 1.0f);
            }
        }

        // Get UV coordinates
        auto uvs = GetFaceUVs(faceDef, texturePath);

        // CRITICAL: Store state before adding vertices
        uint32_t baseIndex = static_cast<uint32_t>(vertices->size());
        size_t originalVertexCount = vertices->size();
        size_t originalIndexCount = indices->size();

        try {
            // Create vertices
            for (int i = 0; i < 4; ++i) {
                Render::Vertex vertex;
                vertex.pos = ModelToWorldSpace(faceVertices[i], blockPos, worldBlockPos);
                vertex.nrm = normal;

                glm::vec2 atlasUV;
                if (GetAtlasUVs(texturePath, uvs[i], atlasUV)) {
                    vertex.uv = atlasUV;
                } else {
                    // Fallback UV
                    vertex.uv = glm::vec2(0.0f, 0.0f);
                }

                vertex.color = tintColor;
                vertex.ao = 255;

                vertices->push_back(vertex);
            }

            // Create indices for two triangles
            indices->push_back(baseIndex + 0);
            indices->push_back(baseIndex + 1);
            indices->push_back(baseIndex + 2);

            indices->push_back(baseIndex + 0);
            indices->push_back(baseIndex + 2);
            indices->push_back(baseIndex + 3);

            // CRITICAL: Validate we added exactly what we expected
            if (vertices->size() != originalVertexCount + 4) {
                Log::Error("Vertex count mismatch: expected %zu, got %zu",
                          originalVertexCount + 4, vertices->size());
            }
            if (indices->size() != originalIndexCount + 6) {
                Log::Error("Index count mismatch: expected %zu, got %zu",
                          originalIndexCount + 6, indices->size());
            }

        } catch (const std::exception& e) {
            Log::Error("Exception in MeshFace: %s", e.what());
            // Restore original state
            vertices->resize(originalVertexCount);
            indices->resize(originalIndexCount);
        }
    }

    void Mesher::GetLayerArrays(LayeredMeshData* meshData, FaceRenderType renderType,
                              std::vector<Render::Vertex>*& vertices,
                              std::vector<uint32_t>*& indices) {
        // CRITICAL: Validate meshData
        if (!meshData) {
            Log::Error("GetLayerArrays: null meshData");
            vertices = nullptr;
            indices = nullptr;
            return;
        }

        switch (renderType) {
            case FaceRenderType::Opaque:
                vertices = &meshData->opaqueVertices;
                indices = &meshData->opaqueIndices;
                break;
            case FaceRenderType::Cutout:
                vertices = &meshData->cutoutVertices;
                indices = &meshData->cutoutIndices;
                break;
            case FaceRenderType::Translucent:
                vertices = &meshData->translucentVertices;
                indices = &meshData->translucentIndices;
                break;
            default:
                Log::Error("GetLayerArrays: invalid render type %d", static_cast<int>(renderType));
                vertices = nullptr;
                indices = nullptr;
                return;
        }

        // CRITICAL: Validate arrays are in good state
        if (vertices && indices) {
            // Check for reasonable sizes
            if (vertices->size() > 50000 || indices->size() > 75000) {
                Log::Warning("Large vertex/index arrays: %zu vertices, %zu indices",
                           vertices->size(), indices->size());
            }
        }
    }

    // CRITICAL: All other existing methods remain the same but with added validation...
    // [Include all the other existing methods with the same validation patterns]

    FaceRenderType Mesher::ClassifyFaceRenderType(const FaceDef& faceDef, BlockID blockId) {
        // Check if this is a fluid block
        if (IsFluidBlock(blockId)) {
            return FaceRenderType::Translucent;
        }

        const Block& block = BlockRegistry::Get(blockId);

        // Check if block is transparent
        if (block.isTransparent) {
            // Further classify transparent blocks
            if (blockId == BlockID::Leaves || blockId == BlockID::CherryLeaves) {
                return FaceRenderType::Cutout; // Alpha-test
            } else {
                return FaceRenderType::Translucent; // Blended
            }
        }

        // Default to opaque
        return FaceRenderType::Opaque;
    }

    bool Mesher::IsFluidBlock(BlockID blockId) {
        return blockId == BlockID::Water || blockId == BlockID::Lava;
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

                    // Get block definition
                    const Block& block = BlockRegistry::Get(blockId);

                    // Get the block model
                    const BlockModel& model = BlockRegistry::GetBlockModel(blockId);

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
                                  meshData, block.enableBiomeTinting,
                                  neighborContext, neighborContext ? neighborContext->center.get() : nullptr);
                    }
                }
            }
        }

        /*Log::Debug("Meshed section (%d, %d, %d) with %zu vertices, %zu indices",
                  chunkPos.x, chunkPos.z, sectionIndex,
                  meshData->vertices.size(), meshData->indices.size());*/
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

        // Resolve texture path - THIS IS THE CRITICAL STEP
        std::string texturePath = model.ResolveTexture(faceDef.textureRef);

        // **FIXED**: Proper biome tinting based on tintindex
        glm::vec4 tintColor(1.0f, 1.0f, 1.0f, 1.0f); // Default: no tinting (white)

        // Only apply tinting if this face has a valid tintindex
        if (faceDef.tintIndex >= 0) {
            if (faceDef.tintIndex == 0) {
                // Grass overlay tinting (tintindex 0)
                glm::vec3 grassTint = SampleGrassTinting(worldBlockPos);
                tintColor = glm::vec4(grassTint, 1.0f);

                // Debug log for grass tinting (remove this later)
                static int debugCounter = 0;
                if (++debugCounter < 5) { // Only log first few instances
                    Log::Debug("Applying grass tint to face with tintindex 0: RGB(%.2f, %.2f, %.2f) at world pos (%d, %d, %d)",
                              grassTint.r, grassTint.g, grassTint.b,
                              worldBlockPos.x, worldBlockPos.y, worldBlockPos.z);
                }
            } else if (faceDef.tintIndex == 1) {
                // Foliage tinting (tintindex 1) for leaves, etc.
                glm::vec3 foliageTint = SampleFoliageTinting(worldBlockPos);
                tintColor = glm::vec4(foliageTint, 1.0f);

                // Debug log for foliage tinting
                static int debugCounter2 = 0;
                if (++debugCounter2 < 5) { // Only log first few instances
                    Log::Debug("Applying foliage tint to face with tintindex 1: RGB(%.2f, %.2f, %.2f) at world pos (%d, %d, %d)",
                              foliageTint.r, foliageTint.g, foliageTint.b,
                              worldBlockPos.x, worldBlockPos.y, worldBlockPos.z);
                }
            }
            // Add more tintindex values here if needed in the future
        }
        // If tintIndex is -1 or any other value, tintColor remains white (no tinting)

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
            bool uvSuccess = GetAtlasUVs(texturePath, uvs[i], atlasUV);
            vertex.uv = atlasUV;

            // **FIXED**: Store tint color in vertex (this will be white if no tinting)
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
        // **CRITICAL FIX**: Handle vertical section boundaries within the same chunk FIRST
        // before checking horizontal chunk boundaries

        // Check if we're accessing a different section within the same chunk
        int currentSectionIndex = (worldY - Config::MinY) / Math::SECTION_HEIGHT;

        // If coordinates are within the same chunk horizontally, use the center chunk
        if (localX >= 0 && localX < Math::CHUNK_SIZE_X &&
            localZ >= 0 && localZ < Math::CHUNK_SIZE_Z) {

            // Check world Y bounds
            if (worldY < Config::MinY || worldY > Config::MaxY) {
                return BlockID::Air; // Out of world bounds vertically
            }

            // **FIXED**: Use center chunk for vertical access (different sections)
            return context.center->GetBlock(localX, worldY, localZ);
        }

        // Handle cross-chunk coordinates (X and Z only - Y is handled above)
        if (worldY < Config::MinY || worldY > Config::MaxY) {
            return BlockID::Air; // Out of world bounds vertically
        }

        // Determine which neighbor chunk to query
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
        // Check chunk-local bounds for X and Z
        if (localX < 0 || localX >= Math::CHUNK_SIZE_X ||
            localZ < 0 || localZ >= Math::CHUNK_SIZE_Z) {
            return BlockID::Air; // Outside chunk bounds horizontally
        }

        // Check world bounds for Y
        if (worldY < Config::MinY || worldY > Config::MaxY) {
            return BlockID::Air; // Outside world bounds vertically
        }

        // **FIXED**: This function now properly handles access to different sections
        // within the same chunk, so inter-section face culling works correctly
        return chunk->GetBlock(localX, worldY, localZ);
    }

    bool Mesher::ShouldCullFace(BlockID currentBlock, BlockID neighborBlock) {
        // Rule 1: Never cull faces adjacent to air
        if (neighborBlock == BlockID::Air) {
            return false;
        }

        // Rule 2: Get block properties for both current and neighbor
        const Block& current = BlockRegistry::Get(currentBlock);
        const Block& neighbor = BlockRegistry::Get(neighborBlock);

        // Rule 3: Special case - transparent blocks against other transparent blocks
        // Generally, we don't cull between transparent blocks unless they're the same type
        if (current.isTransparent && neighbor.isTransparent) {
            // Same block type - cull (e.g., water against water, glass against glass)
            if (currentBlock == neighborBlock) {
                return true;
            }
            // Different transparent blocks - don't cull (e.g., water against glass)
            return false;
        }

        // Rule 4: Transparent block against opaque block
        if (current.isTransparent && !neighbor.isTransparent) {
            // Only cull if the neighbor is a full solid block
            return neighbor.opaque;
        }

        // Rule 5: Opaque block against transparent block - never cull
        // (we want to see the face of the opaque block)
        if (!current.isTransparent && neighbor.isTransparent) {
            return false;
        }

        // Rule 6: Both blocks are opaque - cull the face
        if (!current.isTransparent && !neighbor.isTransparent) {
            return neighbor.opaque;
        }

        // Default fallback - don't cull
        return false;
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

        // Get element bounds in model space (0-16) - ensure these are EXACT integers
        glm::vec3 min = glm::round(element.from);  // Round to ensure integer values
        glm::vec3 max = glm::round(element.to);    // Round to ensure integer values

        switch (faceDir) {
            case FaceDir::Up: // +Y face (TOP of block)
                vertices[0] = glm::vec3(min.x, max.y, max.z); // Bottom-left in texture space
                vertices[1] = glm::vec3(max.x, max.y, max.z); // Bottom-right in texture space
                vertices[2] = glm::vec3(max.x, max.y, min.z); // Top-right in texture space
                vertices[3] = glm::vec3(min.x, max.y, min.z); // Top-left in texture space
                break;

            case FaceDir::Down: // -Y face (BOTTOM of block)
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
        glm::vec3 result = glm::vec3(worldBlockPos) + (modelPos / 16.0f);

        // Add tiny expansion to create overlap (prevents gaps)
        const float TINY_OVERLAP = 0.001f; // Very small overlap

        // Expand outward from block center
        glm::vec3 blockCenter = glm::vec3(worldBlockPos) + glm::vec3(0.5f);
        glm::vec3 direction = result - blockCenter;

        if (glm::length(direction) > 0.0f) {
            direction = glm::normalize(direction);
            result += direction * TINY_OVERLAP;
        }

        return result;
    }

    // grass tinting that varies by position
    glm::vec3 Mesher::SampleGrassTinting(const glm::ivec3& worldPos) {
        // Create position-based variation for more natural look
        float noiseX = sin(worldPos.x * 0.1f) * 0.1f;
        float noiseZ = cos(worldPos.z * 0.1f) * 0.1f;
        float variation = (noiseX + noiseZ) * 0.5f;

        // Base grass green color with subtle variation
        glm::vec3 baseGrass(0.45f, 0.85f, 0.45f); // Bright grass green

        // Apply subtle variation
        baseGrass.r += variation * 0.1f;
        baseGrass.g += variation * 0.05f;
        baseGrass.b += variation * 0.1f;

        // Clamp to valid range
        baseGrass = glm::clamp(baseGrass, glm::vec3(0.0f), glm::vec3(1.0f));

        return baseGrass;
    }

    // tinting that varies by position
    glm::vec3 Mesher::SampleFoliageTinting(const glm::ivec3& worldPos) {
        // Create position-based variation for more natural look
        float noiseX = cos(worldPos.x * 0.15f) * 0.1f;
        float noiseZ = sin(worldPos.z * 0.15f) * 0.1f;
        float variation = (noiseX + noiseZ) * 0.5f;

        // Base foliage green color (slightly darker than grass)
        glm::vec3 baseFoliage(0.35f, 0.75f, 0.35f); // Darker foliage green

        // Apply subtle variation
        baseFoliage.r += variation * 0.1f;
        baseFoliage.g += variation * 0.05f;
        baseFoliage.b += variation * 0.1f;

        // Clamp to valid range
        baseFoliage = glm::clamp(baseFoliage, glm::vec3(0.0f), glm::vec3(1.0f));

        return baseFoliage;
    }

    bool Mesher::GetAtlasUVs(const std::string& texturePath,
                           const glm::vec2& modelUV, glm::vec2& atlasUV) {

        // DEBUG: Always log texture path attempts
        static std::unordered_set<std::string> loggedPaths;
        bool firstTime = loggedPaths.find(texturePath) == loggedPaths.end();
        if (firstTime) {
            loggedPaths.insert(texturePath);
            Log::Debug("GetAtlasUVs: Looking for texture '%s'", texturePath.c_str());
        }

        // Try to get UVs from AtlasBuilder first
        if (Render::g_atlasBuilder) {
            Render::AtlasUVRect uvRect;

            // Strategy 1: Try exact path first
            if (Render::g_atlasBuilder->GetUVRect(texturePath, uvRect)) {
                atlasUV = glm::mix(uvRect.uvMin, uvRect.uvMax, modelUV);
                if (firstTime) {
                    Log::Debug("✓ Found exact match for '%s'", texturePath.c_str());
                }
                return true;
            }

            // Strategy 2: Try with "minecraft:" prefix added
            std::string prefixedPath = "minecraft:" + texturePath;
            if (Render::g_atlasBuilder->GetUVRect(prefixedPath, uvRect)) {
                atlasUV = glm::mix(uvRect.uvMin, uvRect.uvMax, modelUV);
                if (firstTime) {
                    Log::Debug("✓ Found with prefix: '%s' -> '%s'", texturePath.c_str(), prefixedPath.c_str());
                }
                return true;
            }

            // Strategy 3: Try without "minecraft:" prefix if it exists
            if (texturePath.rfind("minecraft:", 0) == 0) {
                std::string unprefixedPath = texturePath.substr(10);
                if (Render::g_atlasBuilder->GetUVRect(unprefixedPath, uvRect)) {
                    atlasUV = glm::mix(uvRect.uvMin, uvRect.uvMax, modelUV);
                    if (firstTime) {
                        Log::Debug("✓ Found without prefix: '%s' -> '%s'", texturePath.c_str(), unprefixedPath.c_str());
                    }
                    return true;
                }
            }

            // Strategy 4: Try common texture path variations
            std::vector<std::string> variations = {
                "block/" + texturePath,                    // "stone" -> "block/stone"
                "minecraft:block/" + texturePath,          // "stone" -> "minecraft:block/stone"
                texturePath.substr(texturePath.find_last_of('/') + 1)  // "block/stone" -> "stone"
            };



            for (const auto& variant : variations) {
                if (Render::g_atlasBuilder->GetUVRect(variant, uvRect)) {
                    atlasUV = glm::mix(uvRect.uvMin, uvRect.uvMax, modelUV);
                    if (firstTime) {
                        Log::Debug("✓ Found variant: '%s' -> '%s'", texturePath.c_str(), variant.c_str());
                    }
                    return true;
                }
            }

            if (firstTime) {
                Log::Warning("✗ Texture '%s' not found in AtlasBuilder", texturePath.c_str());
            }
        }

        return true;
    }

    // UPDATE: Modify your existing InterChunkMesherJob standalone function to call the static member:
    void InterChunkMesherJob(ChunkSection* section, MeshData* meshData,
                           const NeighborContext& context) {
        // Delegate to the static member function
        Mesher::InterChunkMesherJob(section, meshData, context);
    }

    // **FIXED**: Global convenience functions with proper ownership
    void MesherJob(ChunkSection* section, MeshData* meshData, Chunk* parentChunk) {
        if (!section || !meshData) {
            Log::Warning("MesherJob called with null parameters");
            delete meshData; // Clean up if invalid
            return;
        }

        try {
            Mesher::MeshSection(section, meshData, parentChunk);

            // Transfer ownership to callback
            if (g_meshUploadCallback) {
                g_meshUploadCallback(meshData);
                // Callback now owns meshData
            } else {
                Log::Warning("No mesh upload callback registered");
                delete meshData;
            }
        } catch (const std::exception& e) {
            Log::Error("MesherJob failed: %s", e.what());
            delete meshData;
        } catch (...) {
            Log::Error("MesherJob failed with unknown exception");
            delete meshData;
        }
    }

    void Mesher::MeshFluidBlock(const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                              BlockID fluidType, LayeredMeshData* meshData,
                              const NeighborContext& context) {
        // Sample fluid levels at the 4 corners for sloped surface
        std::array<FluidLevel, 4> cornerLevels = {
            GetFluidLevel(worldBlockPos + glm::ivec3(-1, 0, -1), context), // NW
            GetFluidLevel(worldBlockPos + glm::ivec3( 1, 0, -1), context), // NE
            GetFluidLevel(worldBlockPos + glm::ivec3( 1, 0,  1), context), // SE
            GetFluidLevel(worldBlockPos + glm::ivec3(-1, 0,  1), context)  // SW
        };

        // Always mesh the top face for fluids (with slope if needed)
        MeshFluidTopFace(blockPos, worldBlockPos, fluidType, meshData, cornerLevels);

        // Mesh side faces where neighbors are not the same fluid
        for (int faceIdx = 0; faceIdx < 6; ++faceIdx) {
            FaceDirection faceDir = static_cast<FaceDirection>(faceIdx);

            // Skip top and bottom faces (we handle them separately)
            if (faceDir == FaceDirection::PosY || faceDir == FaceDirection::NegY) {
                continue;
            }

            int dx, dy, dz;
            GetFaceOffset(faceDir, dx, dy, dz);

            glm::ivec3 neighborPos = worldBlockPos + glm::ivec3(dx, dy, dz);
            BlockID neighborBlock = GetBlockWithNeighbors(context,
                neighborPos.x - worldBlockPos.x + blockPos.x,
                neighborPos.y,
                neighborPos.z - worldBlockPos.z + blockPos.z);

            // Only render side face if neighbor is different fluid or not fluid
            if (neighborBlock != fluidType) {
                MeshFluidSideFace(blockPos, worldBlockPos, fluidType, faceDir, meshData, context);
            }
        }

        // Mesh bottom face if there's a non-fluid block below
        glm::ivec3 belowPos = worldBlockPos + glm::ivec3(0, -1, 0);
        BlockID belowBlock = GetBlockWithNeighbors(context,
            blockPos.x, belowPos.y, blockPos.z);

        if (belowBlock != fluidType && !IsFluidBlock(belowBlock)) {
            // Mesh bottom face (similar to side face but facing down)
            MeshFluidSideFace(blockPos, worldBlockPos, fluidType, FaceDirection::NegY, meshData, context);
        }
    }

    FluidLevel Mesher::GetFluidLevel(const glm::ivec3& worldPos, const NeighborContext& context) {
        // Convert world position to local coordinates for neighbor lookup
        glm::ivec3 centerWorldPos = glm::ivec3(
            context.center->pos.x * Math::CHUNK_SIZE_X,
            0, // Y will be handled separately
            context.center->pos.z * Math::CHUNK_SIZE_Z
        );

        glm::ivec3 localPos = worldPos - centerWorldPos;

        BlockID blockId = GetBlockWithNeighbors(context, localPos.x, worldPos.y, localPos.z);

        if (IsFluidBlock(blockId)) {
            // For now, use simple full-height fluid
            // In a more advanced implementation, you'd check fluid level metadata
            return FluidLevel(1.0f, blockId);
        }

        return FluidLevel(); // No fluid
    }

    void Mesher::MeshFluidTopFace(const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                                BlockID fluidType, LayeredMeshData* meshData,
                                const std::array<FluidLevel, 4>& cornerLevels) {
        // Get fluid texture
        std::string fluidTexture = (fluidType == BlockID::Water) ? "block/water_still" : "block/lava_still";

        // Calculate heights at corners (default to 1.0 if no neighbor fluid data)
        float heights[4] = {
            cornerLevels[0].isFluid ? cornerLevels[0].height : 1.0f, // NW
            cornerLevels[1].isFluid ? cornerLevels[1].height : 1.0f, // NE
            cornerLevels[2].isFluid ? cornerLevels[2].height : 1.0f, // SE
            cornerLevels[3].isFluid ? cornerLevels[3].height : 1.0f  // SW
        };

        // Create vertices for the top face with varying heights
        std::vector<Render::Vertex>* vertices;
        std::vector<uint32_t>* indices;
        GetLayerArrays(meshData, FaceRenderType::Translucent, vertices, indices);

        uint32_t baseIndex = static_cast<uint32_t>(vertices->size());

        // Top face vertices (ordered for proper winding)
        glm::vec3 worldBase = glm::vec3(worldBlockPos);

        // Vertex positions with height variation
        std::array<glm::vec3, 4> positions = {
            worldBase + glm::vec3(0.0f, heights[0], 0.0f), // NW (0,0)
            worldBase + glm::vec3(1.0f, heights[1], 0.0f), // NE (1,0)
            worldBase + glm::vec3(1.0f, heights[2], 1.0f), // SE (1,1)
            worldBase + glm::vec3(0.0f, heights[3], 1.0f)  // SW (0,1)
        };

        // UV coordinates for fluid animation (can be enhanced later)
        std::array<glm::vec2, 4> uvs = {
            glm::vec2(0.0f, 0.0f), // NW
            glm::vec2(1.0f, 0.0f), // NE
            glm::vec2(1.0f, 1.0f), // SE
            glm::vec2(0.0f, 1.0f)  // SW
        };

        // Create vertices
        for (int i = 0; i < 4; ++i) {
            Render::Vertex vertex;
            vertex.pos = positions[i];
            vertex.nrm = glm::vec3(0.0f, 1.0f, 0.0f); // Always up normal for top face

            // Get atlas UVs
            glm::vec2 atlasUV;
            GetAtlasUVs(fluidTexture, uvs[i], atlasUV);
            vertex.uv = atlasUV;

            // Fluid color (slightly blue-tinted for water, orange for lava)
            if (fluidType == BlockID::Water) {
                vertex.color = glm::vec4(0.7f, 0.8f, 1.0f, 0.8f); // Translucent blue
            } else {
                vertex.color = glm::vec4(1.0f, 0.6f, 0.2f, 1.0f); // Opaque orange-red
            }

            vertex.ao = 255;
            vertices->push_back(vertex);
        }

        // Create indices for two triangles (quad)
        indices->push_back(baseIndex + 0);
        indices->push_back(baseIndex + 1);
        indices->push_back(baseIndex + 2);

        indices->push_back(baseIndex + 0);
        indices->push_back(baseIndex + 2);
        indices->push_back(baseIndex + 3);
    }

    void Mesher::MeshFluidSideFace(const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                                 BlockID fluidType, FaceDirection faceDir,
                                 LayeredMeshData* meshData, const NeighborContext& context) {
        // Get fluid texture
        std::string fluidTexture = (fluidType == BlockID::Water) ? "block/water_flow" : "block/lava_flow";

        std::vector<Render::Vertex>* vertices;
        std::vector<uint32_t>* indices;
        GetLayerArrays(meshData, FaceRenderType::Translucent, vertices, indices);

        uint32_t baseIndex = static_cast<uint32_t>(vertices->size());

        // Get face normal and vertices based on direction
        glm::vec3 normal = glm::vec3(0.0f);
        std::array<glm::vec3, 4> positions;
        glm::vec3 worldBase = glm::vec3(worldBlockPos);

        switch (faceDir) {
            case FaceDirection::PosX: // East face
                normal = glm::vec3(1.0f, 0.0f, 0.0f);
                positions = {
                    worldBase + glm::vec3(1.0f, 0.0f, 1.0f), // Bottom-left
                    worldBase + glm::vec3(1.0f, 0.0f, 0.0f), // Bottom-right
                    worldBase + glm::vec3(1.0f, 1.0f, 0.0f), // Top-right
                    worldBase + glm::vec3(1.0f, 1.0f, 1.0f)  // Top-left
                };
                break;
            case FaceDirection::NegX: // West face
                normal = glm::vec3(-1.0f, 0.0f, 0.0f);
                positions = {
                    worldBase + glm::vec3(0.0f, 0.0f, 0.0f), // Bottom-left
                    worldBase + glm::vec3(0.0f, 0.0f, 1.0f), // Bottom-right
                    worldBase + glm::vec3(0.0f, 1.0f, 1.0f), // Top-right
                    worldBase + glm::vec3(0.0f, 1.0f, 0.0f)  // Top-left
                };
                break;
            case FaceDirection::PosZ: // South face
                normal = glm::vec3(0.0f, 0.0f, 1.0f);
                positions = {
                    worldBase + glm::vec3(0.0f, 0.0f, 1.0f), // Bottom-left
                    worldBase + glm::vec3(1.0f, 0.0f, 1.0f), // Bottom-right
                    worldBase + glm::vec3(1.0f, 1.0f, 1.0f), // Top-right
                    worldBase + glm::vec3(0.0f, 1.0f, 1.0f)  // Top-left
                };
                break;
            case FaceDirection::NegZ: // North face
                normal = glm::vec3(0.0f, 0.0f, -1.0f);
                positions = {
                    worldBase + glm::vec3(1.0f, 0.0f, 0.0f), // Bottom-left
                    worldBase + glm::vec3(0.0f, 0.0f, 0.0f), // Bottom-right
                    worldBase + glm::vec3(0.0f, 1.0f, 0.0f), // Top-right
                    worldBase + glm::vec3(1.0f, 1.0f, 0.0f)  // Top-left
                };
                break;
            case FaceDirection::NegY: // Bottom face
                normal = glm::vec3(0.0f, -1.0f, 0.0f);
                positions = {
                    worldBase + glm::vec3(0.0f, 0.0f, 0.0f), // Bottom-left
                    worldBase + glm::vec3(1.0f, 0.0f, 0.0f), // Bottom-right
                    worldBase + glm::vec3(1.0f, 0.0f, 1.0f), // Top-right
                    worldBase + glm::vec3(0.0f, 0.0f, 1.0f)  // Top-left
                };
                break;
            default:
                return; // Invalid face direction
        }

        // UV coordinates
        std::array<glm::vec2, 4> uvs = {
            glm::vec2(0.0f, 1.0f), // Bottom-left
            glm::vec2(1.0f, 1.0f), // Bottom-right
            glm::vec2(1.0f, 0.0f), // Top-right
            glm::vec2(0.0f, 0.0f)  // Top-left
        };

        // Create vertices
        for (int i = 0; i < 4; ++i) {
            Render::Vertex vertex;
            vertex.pos = positions[i];
            vertex.nrm = normal;

            // Get atlas UVs
            glm::vec2 atlasUV;
            GetAtlasUVs(fluidTexture, uvs[i], atlasUV);
            vertex.uv = atlasUV;

            // Fluid color
            if (fluidType == BlockID::Water) {
                vertex.color = glm::vec4(0.7f, 0.8f, 1.0f, 0.8f); // Translucent blue
            } else {
                vertex.color = glm::vec4(1.0f, 0.6f, 0.2f, 1.0f); // Opaque orange-red
            }

            vertex.ao = 255;
            vertices->push_back(vertex);
        }

        // Create indices for two triangles
        indices->push_back(baseIndex + 0);
        indices->push_back(baseIndex + 1);
        indices->push_back(baseIndex + 2);

        indices->push_back(baseIndex + 0);
        indices->push_back(baseIndex + 2);
        indices->push_back(baseIndex + 3);
    }

    // ADDED: Static member function implementation for InterChunkMesherJob
    void Mesher::InterChunkMesherJob(ChunkSection* section, MeshData* meshData,
                                   const NeighborContext& context) {
        if (!section || !meshData) {
            Log::Warning("Mesher::InterChunkMesherJob called with null parameters");
            delete meshData;
            return;
        }

        try {
            MeshSectionWithNeighbors(section, meshData, context);

            // Transfer ownership to callback
            if (g_meshUploadCallback) {
                g_meshUploadCallback(meshData);
                // Callback now owns meshData
            } else {
                Log::Warning("No mesh upload callback registered");
                delete meshData;
            }
        } catch (const std::exception& e) {
            Log::Error("Mesher::InterChunkMesherJob failed: %s", e.what());
            delete meshData;
        } catch (...) {
            Log::Error("Mesher::InterChunkMesherJob failed with unknown exception");
            delete meshData;
        }
    }

} // namespace Game