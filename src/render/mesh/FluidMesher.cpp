// File: src/render/mesh/FluidMesher.cpp
#include "Mesher.hpp"
#include "../../engine/block/BlockRegistry.hpp"
#include "../../engine/block/BlockModel.hpp"
#include "../atlas/AtlasBuilder.hpp"
#include "../../core/Log.hpp"
#include "../../core/Config.hpp"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace Game {

    // Global layered mesh upload callback
    static LayeredMeshUploadCallback g_layeredMeshUploadCallback = nullptr;

    void SetLayeredMeshUploadCallback(LayeredMeshUploadCallback callback) {
        g_layeredMeshUploadCallback = callback;
    }

    void Mesher::MeshSectionLayered(ChunkSection* section, LayeredMeshData* meshData,
                                  const NeighborContext& context) {
        if (!section || !meshData || !context.center) {
            Log::Warning("Invalid parameters passed to MeshSectionLayered");
            return;
        }

        int sectionIndex = meshData->sectionIndex;
        MeshSectionLayeredInternal(section, meshData, context.center->pos, sectionIndex, context);
    }

    void Mesher::MeshSectionLayeredInternal(ChunkSection* section, LayeredMeshData* meshData,
                                          Math::ChunkPos chunkPos, int sectionIndex,
                                          const NeighborContext& context) {
        // Clear existing data
        meshData->opaqueVertices.clear();
        meshData->opaqueIndices.clear();
        meshData->cutoutVertices.clear();
        meshData->cutoutIndices.clear();
        meshData->translucentVertices.clear();
        meshData->translucentIndices.clear();
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

                    // Calculate positions
                    glm::ivec3 blockPos(x, y, z);  // Local position within section
                    glm::ivec3 worldBlockPos(
                        chunkPos.x * Math::CHUNK_SIZE_X + x,
                        worldYOffset + y,
                        chunkPos.z * Math::CHUNK_SIZE_Z + z
                    );

                    // Mesh this block with layer classification
                    MeshBlockLayered(blockPos, worldBlockPos, blockId, meshData, context);
                }
            }
        }
    }

    void Mesher::MeshBlockLayered(const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                                BlockID blockId, LayeredMeshData* meshData,
                                const NeighborContext& context) {
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

        // Mesh all elements using enhanced element meshing
        for (const auto& element : model.elements) {
            MeshElement(element, model, blockPos, worldBlockPos, blockId,
                       meshData, block.enableBiomeTinting, context);
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

    bool Mesher::IsFluidBlock(BlockID blockId) {
        return blockId == BlockID::Water || blockId == BlockID::Lava;
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

    void Mesher::GetLayerArrays(LayeredMeshData* meshData, FaceRenderType renderType,
                              std::vector<Render::Vertex>*& vertices,
                              std::vector<uint32_t>*& indices) {
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

            // Mesh this face with layer classification
            MeshFace(element, faceDef, faceDir, model, blockPos, worldBlockPos,
                    currentBlockId, meshData, enableBiomeTinting);
        }
    }

    void Mesher::MeshFace(const Element& element, const FaceDef& faceDef,
                        FaceDir faceDir, const BlockModel& model,
                        const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                        BlockID currentBlockId, LayeredMeshData* meshData, bool enableBiomeTinting) {

        // Classify this face's render type
        FaceRenderType renderType = ClassifyFaceRenderType(faceDef, currentBlockId);

        // Get appropriate vertex/index arrays
        std::vector<Render::Vertex>* vertices;
        std::vector<uint32_t>* indices;
        GetLayerArrays(meshData, renderType, vertices, indices);

        // Get face geometry
        glm::vec3 normal = GetFaceNormal(faceDir);
        auto faceVertices = GetFaceVertices(element, faceDir);

        // Resolve texture path
        std::string texturePath = model.ResolveTexture(faceDef.textureRef);

        // Calculate tint color
        glm::vec4 tintColor(1.0f, 1.0f, 1.0f, 1.0f);
        if (faceDef.tintIndex >= 0) {
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

        // Create vertices
        uint32_t baseIndex = static_cast<uint32_t>(vertices->size());

        for (int i = 0; i < 4; ++i) {
            Render::Vertex vertex;
            vertex.pos = ModelToWorldSpace(faceVertices[i], blockPos, worldBlockPos);
            vertex.nrm = normal;

            glm::vec2 atlasUV;
            GetAtlasUVs(texturePath, uvs[i], atlasUV);
            vertex.uv = atlasUV;

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
    }

    // Enhanced convenience function for layered meshing
    void LayeredMesherJob(ChunkSection* section, LayeredMeshData* meshData,
                        const NeighborContext& context) {
        if (!section || !meshData) {
            Log::Warning("LayeredMesherJob called with null parameters");
            delete meshData;
            return;
        }

        try {
            Mesher::MeshSectionLayered(section, meshData, context);

            // Transfer ownership to callback
            if (g_layeredMeshUploadCallback) {
                g_layeredMeshUploadCallback(meshData);
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

} // namespace Game