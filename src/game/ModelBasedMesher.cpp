// File: src/game/ModelBasedMesher.cpp
#include "ModelBasedMesher.hpp"
#include "EnhancedBlockRegistry.hpp"
#include "BlockRegistry.hpp"
#include "../core/Log.hpp"
#include "../core/Config.hpp"
#include <cmath>
#include <algorithm>

namespace Game {

    // Face direction offsets
    const glm::ivec3 ModelBasedMesher::FACE_OFFSETS[6] = {
        { 0,  1,  0},  // Up
        { 0, -1,  0},  // Down
        { 0,  0, -1},  // North
        { 0,  0,  1},  // South
        {-1,  0,  0},  // West
        { 1,  0,  0}   // East
    };

    void ModelBasedMesher::MeshBlockWithModel(
        const BlockModel& model,
        const glm::ivec3& blockPos,
        const ModelMeshContext& context,
        std::vector<Render::Vertex>& vertices,
        std::vector<uint32_t>& indices) {

        if (!context.atlasBuilder) {
            Log::Warning("No AtlasBuilder provided to ModelBasedMesher");
            return;
        }

        // Process each element in the block model
        for (const auto& element : model.elements) {

            // Process each face of the element
            for (const auto& [faceDir, faceDef] : element.faces) {

                // Check if this face should be culled
                if (ShouldCullFace(blockPos, faceDir, faceDef.cullface, context)) {
                    if (context.facesCulled) (*context.facesCulled)++;
                    continue;
                }

                // Get texture UV coordinates from AtlasBuilder
                Render::AtlasUVRect uvRect;
                if (!GetModelTextureUV(faceDef.textureRef, model, *context.atlasBuilder, uvRect)) {
                    Log::Warning("Failed to get UV for texture: %s", faceDef.textureRef.c_str());
                    continue;
                }

                // Calculate face normal and position
                glm::vec3 faceNormal = GetFaceNormal(faceDir);

                // Convert element coordinates from 0-16 model space to 0-1 block space
                glm::vec3 elementMin = element.from / 16.0f;
                glm::vec3 elementMax = element.to / 16.0f;

                // Generate quad vertices and UVs
                std::array<glm::vec3, 4> quadVertices;
                std::array<glm::vec2, 4> quadUVs;

                GenerateQuadForFace(faceDir, elementMin, elementMax,
                                  faceDef.uv, uvRect, quadVertices, quadUVs);

                // Calculate biome tint if needed
                glm::vec3 tintColor(1.0f); // Default: no tint
                if (faceDef.tintIndex >= 0) {
                    tintColor = CalculateBiomeTint(
                        faceDef.tintIndex,
                        context.biomeTemperature,
                        context.biomeHumidity,
                        *context.atlasBuilder
                    );
                }

                // Add vertices to mesh
                uint32_t baseIndex = static_cast<uint32_t>(vertices.size());

                for (int i = 0; i < 4; ++i) {
                    Render::Vertex vertex;
                    vertex.pos = glm::vec3(blockPos) + quadVertices[i];
                    vertex.nrm = faceNormal;
                    vertex.uv = quadUVs[i];
                    vertex.ao = 255; // Full brightness for now

                    // Note: Biome tinting would be applied in the fragment shader
                    // using the tintIndex and colormap textures

                    vertices.push_back(vertex);
                }

                // Add indices for two triangles (counter-clockwise winding)
                indices.insert(indices.end(), {
                    baseIndex + 0, baseIndex + 2, baseIndex + 1,
                    baseIndex + 0, baseIndex + 3, baseIndex + 2
                });

                if (context.facesGenerated) (*context.facesGenerated)++;
            }
        }
    }

    bool ModelBasedMesher::GetModelTextureUV(
        const std::string& textureRef,
        const BlockModel& model,
        const Render::AtlasBuilder& atlas,
        Render::AtlasUVRect& uvRect) {

        // Resolve texture reference through the model
        std::string texturePath = model.ResolveTexture(textureRef);

        // Look up in AtlasBuilder
        return atlas.GetUVRect(texturePath, uvRect);
    }

    glm::vec3 ModelBasedMesher::CalculateBiomeTint(
        int tintIndex,
        int temperature,
        int humidity,
        const Render::AtlasBuilder& atlas) {

        // Default to no tint
        glm::vec3 tintColor(1.0f);

        if (tintIndex < 0) {
            return tintColor;
        }

        // Normalize temperature and humidity to 0-1 range
        float temp = std::clamp(temperature / 255.0f, 0.0f, 1.0f);
        float humid = std::clamp(humidity / 255.0f, 0.0f, 1.0f);

        // Use the appropriate colormap based on tint index
        GLuint colormapID = 0;
        if (tintIndex == 0) {
            colormapID = atlas.GetGrassColormapID();
        } else if (tintIndex == 1) {
            colormapID = atlas.GetFoliageColormapID();
        }

        if (colormapID == 0) {
            return tintColor; // No colormap available
        }

        // For now, return a simple calculated tint
        // TODO: Implement actual colormap texture sampling

        // Simple grass tinting formula (based on Minecraft's algorithm)
        if (tintIndex == 0) {
            // Grass tinting
            float greenness = 0.5f + 0.3f * humid * (1.0f - std::abs(temp - 0.5f));
            tintColor = glm::vec3(0.6f + 0.2f * temp, greenness, 0.4f + 0.2f * humid);
        } else {
            // Foliage tinting
            float leafiness = 0.4f + 0.4f * humid * (1.0f - temp * 0.5f);
            tintColor = glm::vec3(0.5f, leafiness, 0.3f + 0.3f * humid);
        }

        return glm::clamp(tintColor, 0.0f, 1.0f);
    }

    bool ModelBasedMesher::ShouldCullFace(
        const glm::ivec3& blockPos,
        FaceDir faceDir,
        const std::string& cullface,
        const ModelMeshContext& context) {

        // If no cullface is specified, don't cull
        if (cullface.empty()) {
            return false;
        }

        // Parse cullface direction (should match faceDir for standard culling)
        FaceDir cullfaceDir = ParseFaceDir(cullface);
        if (cullfaceDir != faceDir) {
            return false; // Non-standard culling, skip for now
        }

        if (!context.neighborCtx) {
            return false; // Can't cull without neighbor info
        }

        // Calculate neighbor position
        glm::ivec3 neighborPos = blockPos + FACE_OFFSETS[static_cast<int>(faceDir)];

        // Get neighbor block
        BlockID neighborBlock = GetBlockAtPosition(neighborPos, context);

        // Cull if neighbor is opaque
        return IsBlockOpaque(neighborBlock);
    }

    glm::vec3 ModelBasedMesher::GetFaceNormal(FaceDir dir) {
        switch (dir) {
            case FaceDir::Up:    return glm::vec3(0, 1, 0);
            case FaceDir::Down:  return glm::vec3(0, -1, 0);
            case FaceDir::North: return glm::vec3(0, 0, -1);
            case FaceDir::South: return glm::vec3(0, 0, 1);
            case FaceDir::West:  return glm::vec3(-1, 0, 0);
            case FaceDir::East:  return glm::vec3(1, 0, 0);
            default:             return glm::vec3(0, 1, 0);
        }
    }

    void ModelBasedMesher::GenerateQuadForFace(
        FaceDir faceDir,
        const glm::vec3& elementMin,
        const glm::vec3& elementMax,
        const glm::vec4& faceUV,
        const Render::AtlasUVRect& atlasUV,
        std::array<glm::vec3, 4>& quadVertices,
        std::array<glm::vec2, 4>& quadUVs) {

        // Generate vertices based on face direction
        switch (faceDir) {
            case FaceDir::Up: // +Y face
                quadVertices[0] = glm::vec3(elementMin.x, elementMax.y, elementMin.z);
                quadVertices[1] = glm::vec3(elementMax.x, elementMax.y, elementMin.z);
                quadVertices[2] = glm::vec3(elementMax.x, elementMax.y, elementMax.z);
                quadVertices[3] = glm::vec3(elementMin.x, elementMax.y, elementMax.z);
                break;

            case FaceDir::Down: // -Y face
                quadVertices[0] = glm::vec3(elementMin.x, elementMin.y, elementMax.z);
                quadVertices[1] = glm::vec3(elementMax.x, elementMin.y, elementMax.z);
                quadVertices[2] = glm::vec3(elementMax.x, elementMin.y, elementMin.z);
                quadVertices[3] = glm::vec3(elementMin.x, elementMin.y, elementMin.z);
                break;

            case FaceDir::North: // -Z face
                quadVertices[0] = glm::vec3(elementMax.x, elementMin.y, elementMin.z);
                quadVertices[1] = glm::vec3(elementMin.x, elementMin.y, elementMin.z);
                quadVertices[2] = glm::vec3(elementMin.x, elementMax.y, elementMin.z);
                quadVertices[3] = glm::vec3(elementMax.x, elementMax.y, elementMin.z);
                break;

            case FaceDir::South: // +Z face
                quadVertices[0] = glm::vec3(elementMin.x, elementMin.y, elementMax.z);
                quadVertices[1] = glm::vec3(elementMax.x, elementMin.y, elementMax.z);
                quadVertices[2] = glm::vec3(elementMax.x, elementMax.y, elementMax.z);
                quadVertices[3] = glm::vec3(elementMin.x, elementMax.y, elementMax.z);
                break;

            case FaceDir::West: // -X face
                quadVertices[0] = glm::vec3(elementMin.x, elementMin.y, elementMin.z);
                quadVertices[1] = glm::vec3(elementMin.x, elementMin.y, elementMax.z);
                quadVertices[2] = glm::vec3(elementMin.x, elementMax.y, elementMax.z);
                quadVertices[3] = glm::vec3(elementMin.x, elementMax.y, elementMin.z);
                break;

            case FaceDir::East: // +X face
                quadVertices[0] = glm::vec3(elementMax.x, elementMin.y, elementMax.z);
                quadVertices[1] = glm::vec3(elementMax.x, elementMin.y, elementMin.z);
                quadVertices[2] = glm::vec3(elementMax.x, elementMax.y, elementMin.z);
                quadVertices[3] = glm::vec3(elementMax.x, elementMax.y, elementMax.z);
                break;
        }

        // Map face UV coordinates to atlas UV coordinates
        // faceUV is in 0-16 pixel space, we need to map to atlas UV space
        float u1 = faceUV.x / 16.0f;
        float v1 = faceUV.y / 16.0f;
        float u2 = faceUV.z / 16.0f;
        float v2 = faceUV.w / 16.0f;

        // Map to atlas coordinates
        float atlasU1 = atlasUV.uvMin.x + u1 * (atlasUV.uvMax.x - atlasUV.uvMin.x);
        float atlasV1 = atlasUV.uvMin.y + v1 * (atlasUV.uvMax.y - atlasUV.uvMin.y);
        float atlasU2 = atlasUV.uvMin.x + u2 * (atlasUV.uvMax.x - atlasUV.uvMin.x);
        float atlasV2 = atlasUV.uvMin.y + v2 * (atlasUV.uvMax.y - atlasUV.uvMin.y);

        // Generate UV coordinates (corrected for OpenGL texture coordinate system)
        quadUVs[0] = glm::vec2(atlasU1, atlasV2); // Bottom-left
        quadUVs[1] = glm::vec2(atlasU2, atlasV2); // Bottom-right
        quadUVs[2] = glm::vec2(atlasU2, atlasV1); // Top-right
        quadUVs[3] = glm::vec2(atlasU1, atlasV1); // Top-left
    }

    BlockID ModelBasedMesher::GetBlockAtPosition(
        const glm::ivec3& pos,
        const ModelMeshContext& context) {

        if (!context.neighborCtx) {
            return BlockID::Air;
        }

        // Convert to local chunk coordinates for the neighbor context lookup
        // This is similar to the existing GetBlockFromNeighborContext function
        int localX = pos.x;
        int worldY = pos.y;
        int localZ = pos.z;

        // Handle within-chunk coordinates (most common case)
        if (localX >= 0 && localX < Math::CHUNK_SIZE_X &&
            localZ >= 0 && localZ < Math::CHUNK_SIZE_Z &&
            worldY >= Config::MinY && worldY <= Config::MaxY) {
            return context.neighborCtx->center->GetBlock(localX, worldY, localZ);
        }

        // Handle cross-chunk coordinates
        std::shared_ptr<Chunk> targetChunk = nullptr;
        int targetX = localX;
        int targetZ = localZ;

        // Determine which neighbor to use and adjust coordinates
        if (localX < 0 && localZ >= 0 && localZ < Math::CHUNK_SIZE_Z) {
            // West neighbor
            if (context.neighborCtx->neighbors.size() > 0) {
                targetChunk = context.neighborCtx->neighbors[0];
                targetX = localX + Math::CHUNK_SIZE_X;
            }
        } else if (localX >= Math::CHUNK_SIZE_X && localZ >= 0 && localZ < Math::CHUNK_SIZE_Z) {
            // East neighbor
            if (context.neighborCtx->neighbors.size() > 1) {
                targetChunk = context.neighborCtx->neighbors[1];
                targetX = localX - Math::CHUNK_SIZE_X;
            }
        } else if (localZ < 0 && localX >= 0 && localX < Math::CHUNK_SIZE_X) {
            // North neighbor
            if (context.neighborCtx->neighbors.size() > 2) {
                targetChunk = context.neighborCtx->neighbors[2];
                targetZ = localZ + Math::CHUNK_SIZE_Z;
            }
        } else if (localZ >= Math::CHUNK_SIZE_Z && localX >= 0 && localX < Math::CHUNK_SIZE_X) {
            // South neighbor
            if (context.neighborCtx->neighbors.size() > 3) {
                targetChunk = context.neighborCtx->neighbors[3];
                targetZ = localZ - Math::CHUNK_SIZE_Z;
            }
        }

        // Return air if neighbor isn't available or coordinates are out of bounds
        if (!targetChunk ||
            targetX < 0 || targetX >= Math::CHUNK_SIZE_X ||
            targetZ < 0 || targetZ >= Math::CHUNK_SIZE_Z ||
            worldY < Config::MinY || worldY > Config::MaxY) {
            return BlockID::Air;
        }

        return targetChunk->GetBlock(targetX, worldY, targetZ);
    }

    bool ModelBasedMesher::IsBlockOpaque(BlockID blockId) {
        if (blockId == BlockID::Air) {
            return false;
        }

        const EnhancedBlock& block = EnhancedBlockRegistry::Get(blockId);
        return block.opaque;
    }

} // namespace Game