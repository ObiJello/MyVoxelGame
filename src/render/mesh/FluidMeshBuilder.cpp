// File: src/render/mesh/FluidMeshBuilder.cpp
#include "FluidMeshBuilder.hpp"
#include "../../core/Log.hpp"
#include "../../core/Config.hpp"
#include <chrono>
#include <algorithm>

namespace Render {

    FluidMeshBuilder::FluidMeshBuilder(Game::World& world, AtlasBuilder& atlas)
        : world(world), atlas(atlas) {

        // Set default configuration
        config = FluidConfig{};

        Log::Debug("FluidMeshBuilder initialized");
    }

    void FluidMeshBuilder::AppendFluidQuads(int chunkX, int chunkZ, ChunkMeshData& meshData) {
        auto startTime = std::chrono::high_resolution_clock::now();

        // Reset statistics
        lastStats = FluidStats{};

        Log::Debug("Building fluid geometry for chunk (%d, %d)", chunkX, chunkZ);

        // Process all fluid blocks in the chunk
        ProcessChunkFluids(chunkX, chunkZ, meshData);

        // Calculate timing
        auto endTime = std::chrono::high_resolution_clock::now();
        lastStats.buildTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        if (lastStats.fluidsProcessed > 0) {
            Log::Debug("Chunk (%d, %d) fluid geometry: %d fluids, %d top quads, %d side quads, %.2fms",
                      chunkX, chunkZ, lastStats.fluidsProcessed,
                      lastStats.topQuadsGenerated, lastStats.sideQuadsGenerated, lastStats.buildTimeMs);
        }
    }

    void FluidMeshBuilder::ProcessChunkFluids(int chunkX, int chunkZ, ChunkMeshData& meshData) {
        // Calculate world coordinates for this chunk
        int baseWorldX = chunkX * Game::Math::CHUNK_SIZE_X;
        int baseWorldZ = chunkZ * Game::Math::CHUNK_SIZE_Z;

        // Iterate through all blocks in the chunk volume
        for (int localX = 0; localX < Game::Math::CHUNK_SIZE_X; ++localX) {
            for (int localZ = 0; localZ < Game::Math::CHUNK_SIZE_Z; ++localZ) {
                for (int worldY = Config::MinY; worldY <= Config::MaxY; ++worldY) {
                    int worldX = baseWorldX + localX;
                    int worldZ = baseWorldZ + localZ;

                    // Get block at this position
                    Game::BlockID blockId = world.GetBlock(worldX, worldY, worldZ);

                    // Process fluid blocks
                    if (blockId == Game::BlockID::Water || blockId == Game::BlockID::Lava) {
                        ProcessFluidBlock(worldX, worldY, worldZ, blockId, meshData);
                        lastStats.fluidsProcessed++;
                    }
                }
            }
        }
    }

    void FluidMeshBuilder::ProcessFluidBlock(int worldX, int worldY, int worldZ,
                                           Game::BlockID fluidType, ChunkMeshData& meshData) {
        // Calculate corner heights for sloped fluid surface
        std::array<float, 4> cornerHeights = CalculateCornerHeights(worldX, worldY, worldZ, fluidType);

        // Generate top surface quad
        if (config.generateTopQuads) {
            GenerateFluidTopQuad(worldX, worldY, worldZ, fluidType, cornerHeights, meshData);
            lastStats.topQuadsGenerated++;
        }

        // Generate side quads where fluid meets non-fluid
        if (config.generateSideQuads) {
            GenerateFluidSideQuads(worldX, worldY, worldZ, fluidType, cornerHeights, meshData);
        }
    }

    std::array<float, 4> FluidMeshBuilder::CalculateCornerHeights(int worldX, int worldY, int worldZ,
                                                                 Game::BlockID fluidType) {
        std::array<float, 4> heights;

        // Sample height at each corner of the block
        for (int i = 0; i < 4; ++i) {
            int cornerX = CORNER_OFFSETS[i][0];
            int cornerZ = CORNER_OFFSETS[i][1];

            heights[i] = SampleCornerHeight(worldX, worldY, worldZ, cornerX, cornerZ, fluidType);
        }

        return heights;
    }

    float FluidMeshBuilder::SampleCornerHeight(int worldX, int worldY, int worldZ,
                                              int cornerX, int cornerZ, Game::BlockID fluidType) {
        // Sample surrounding fluid blocks to calculate corner height
        float totalHeight = 0.0f;
        int sampleCount = 0;

        // Sample in a 2x2 grid around the corner
        for (int dx = 0; dx <= 1; ++dx) {
            for (int dz = 0; dz <= 1; ++dz) {
                int sampleX = worldX + cornerX + dx - 1;
                int sampleZ = worldZ + cornerZ + dz - 1;

                FluidLevel level = GetFluidLevel(sampleX, worldY, sampleZ, fluidType);

                if (level.isFluid && level.fluidType == fluidType) {
                    totalHeight += level.height;
                    sampleCount++;
                }
            }
        }

        if (sampleCount > 0) {
            return totalHeight / sampleCount;
        } else {
            // No fluid found, use default height
            return GetDefaultFluidHeight(fluidType);
        }
    }

    FluidLevel FluidMeshBuilder::GetFluidLevel(int x, int y, int z, Game::BlockID expectedFluid) {
        FluidLevel level;

        Game::BlockID blockId = world.GetBlock(x, y, z);

        if (blockId == expectedFluid) {
            level.isFluid = true;
            level.fluidType = blockId;

            // Check if there's fluid above (makes this a "full" fluid block)
            Game::BlockID above = world.GetBlock(x, y + 1, z);
            if (above == expectedFluid) {
                level.height = MAX_FLUID_HEIGHT;
            } else {
                // Use default height for surface fluid
                level.height = GetDefaultFluidHeight(blockId);
            }
        } else {
            level.isFluid = false;
            level.height = 0.0f;
        }

        return level;
    }

    void FluidMeshBuilder::GenerateFluidTopQuad(int worldX, int worldY, int worldZ,
                                               Game::BlockID fluidType,
                                               const std::array<float, 4>& cornerHeights,
                                               ChunkMeshData& meshData) {
        // Check if we should render the top surface
        Game::BlockID above = world.GetBlock(worldX, worldY + 1, worldZ);
        if (above == fluidType) {
            return; // Don't render top if same fluid is above
        }

        // Generate positions with corner heights
        std::vector<glm::vec3> positions;
        positions.reserve(4);

        // Bottom-left
        positions.push_back(glm::vec3(worldX, worldY + cornerHeights[0], worldZ));
        // Bottom-right
        positions.push_back(glm::vec3(worldX + 1, worldY + cornerHeights[1], worldZ));
        // Top-right
        positions.push_back(glm::vec3(worldX + 1, worldY + cornerHeights[2], worldZ + 1));
        // Top-left
        positions.push_back(glm::vec3(worldX, worldY + cornerHeights[3], worldZ + 1));

        // Get fluid texture
        AtlasUVRect uvRect;
        bool isStill = !IsFluidFlowing(worldX, worldY, worldZ, fluidType);
        if (!GetFluidTexture(fluidType, isStill, uvRect)) {
            Log::Warning("Failed to get fluid texture for %d", static_cast<int>(fluidType));
            return;
        }

        // Generate UVs
        std::vector<glm::vec2> uvs = GenerateFluidUVs(uvRect, true);

        // Get fluid color
        glm::vec4 color = GetFluidColor(fluidType);

        // Face normal (always up for top surface)
        glm::vec3 normal(0.0f, 1.0f, 0.0f);

        // Add to translucent layer
        AddFluidQuadVertices(positions, normal, uvs, color, meshData.translucent);
        lastStats.verticesGenerated += 4;
    }

    void FluidMeshBuilder::GenerateFluidSideQuads(int worldX, int worldY, int worldZ,
                                                 Game::BlockID fluidType,
                                                 const std::array<float, 4>& cornerHeights,
                                                 ChunkMeshData& meshData) {
        // Check each horizontal direction
        std::array<Game::FaceDir, 4> horizontalFaces = {
            Game::FaceDir::North, Game::FaceDir::South,
            Game::FaceDir::West, Game::FaceDir::East
        };

        for (Game::FaceDir faceDir : horizontalFaces) {
            if (ShouldRenderFluidSide(worldX, worldY, worldZ, faceDir, fluidType)) {
                GenerateFluidSideQuad(worldX, worldY, worldZ, fluidType, faceDir, cornerHeights, meshData);
                lastStats.sideQuadsGenerated++;
            }
        }
    }

    void FluidMeshBuilder::GenerateFluidSideQuad(int worldX, int worldY, int worldZ,
                                                Game::BlockID fluidType, Game::FaceDir faceDir,
                                                const std::array<float, 4>& cornerHeights,
                                                ChunkMeshData& meshData) {
        std::vector<glm::vec3> positions;
        positions.reserve(4);

        float baseY = static_cast<float>(worldY);
        float topHeight = GetDefaultFluidHeight(fluidType);

        // Generate positions based on face direction
        switch (faceDir) {
            case Game::FaceDir::North: // -Z
                positions.push_back(glm::vec3(worldX + 1, baseY, worldZ));
                positions.push_back(glm::vec3(worldX, baseY, worldZ));
                positions.push_back(glm::vec3(worldX, baseY + topHeight, worldZ));
                positions.push_back(glm::vec3(worldX + 1, baseY + topHeight, worldZ));
                break;

            case Game::FaceDir::South: // +Z
                positions.push_back(glm::vec3(worldX, baseY, worldZ + 1));
                positions.push_back(glm::vec3(worldX + 1, baseY, worldZ + 1));
                positions.push_back(glm::vec3(worldX + 1, baseY + topHeight, worldZ + 1));
                positions.push_back(glm::vec3(worldX, baseY + topHeight, worldZ + 1));
                break;

            case Game::FaceDir::West: // -X
                positions.push_back(glm::vec3(worldX, baseY, worldZ));
                positions.push_back(glm::vec3(worldX, baseY, worldZ + 1));
                positions.push_back(glm::vec3(worldX, baseY + topHeight, worldZ + 1));
                positions.push_back(glm::vec3(worldX, baseY + topHeight, worldZ));
                break;

            case Game::FaceDir::East: // +X
                positions.push_back(glm::vec3(worldX + 1, baseY, worldZ + 1));
                positions.push_back(glm::vec3(worldX + 1, baseY, worldZ));
                positions.push_back(glm::vec3(worldX + 1, baseY + topHeight, worldZ));
                positions.push_back(glm::vec3(worldX + 1, baseY + topHeight, worldZ + 1));
                break;

            default:
                return; // Invalid face direction for side quad
        }

        // Get fluid texture (use flow texture for sides)
        AtlasUVRect uvRect;
        if (!GetFluidTexture(fluidType, false, uvRect)) {
            return;
        }

        // Generate UVs
        std::vector<glm::vec2> uvs = GenerateFluidUVs(uvRect, false);

        // Get fluid color
        glm::vec4 color = GetFluidColor(fluidType);

        // Calculate face normal
        glm::vec3 normal;
        switch (faceDir) {
            case Game::FaceDir::North: normal = glm::vec3(0.0f, 0.0f, -1.0f); break;
            case Game::FaceDir::South: normal = glm::vec3(0.0f, 0.0f, 1.0f); break;
            case Game::FaceDir::West:  normal = glm::vec3(-1.0f, 0.0f, 0.0f); break;
            case Game::FaceDir::East:  normal = glm::vec3(1.0f, 0.0f, 0.0f); break;
            default: normal = glm::vec3(0.0f, 1.0f, 0.0f); break;
        }

        // Add to translucent layer
        AddFluidQuadVertices(positions, normal, uvs, color, meshData.translucent);
        lastStats.verticesGenerated += 4;
    }

    bool FluidMeshBuilder::ShouldRenderFluidSide(int worldX, int worldY, int worldZ,
                                                 Game::FaceDir faceDir, Game::BlockID fluidType) {
        // Get neighbor position
        glm::ivec3 offset;
        switch (faceDir) {
            case Game::FaceDir::North: offset = glm::ivec3(0, 0, -1); break;
            case Game::FaceDir::South: offset = glm::ivec3(0, 0, 1); break;
            case Game::FaceDir::West:  offset = glm::ivec3(-1, 0, 0); break;
            case Game::FaceDir::East:  offset = glm::ivec3(1, 0, 0); break;
            default: return false;
        }

        Game::BlockID neighbor = world.GetBlock(worldX + offset.x, worldY + offset.y, worldZ + offset.z);

        // Render side if neighbor is not the same fluid type
        return neighbor != fluidType;
    }

    bool FluidMeshBuilder::GetFluidTexture(Game::BlockID fluidType, bool isStill, AtlasUVRect& uvRect) {
        std::string textureName = GetFluidTextureName(fluidType, isStill);
        return atlas.GetUVRect(textureName, uvRect);
    }

    std::string FluidMeshBuilder::GetFluidTextureName(Game::BlockID fluidType, bool isStill) {
        switch (fluidType) {
            case Game::BlockID::Water:
                return isStill ? "block/water_still" : "block/water_flow";
            case Game::BlockID::Lava:
                return isStill ? "block/lava_still" : "block/lava_flow";
            default:
                return "missingno";
        }
    }

    bool FluidMeshBuilder::IsFluidFlowing(int worldX, int worldY, int worldZ, Game::BlockID fluidType) {
        // Simple flow detection: check if any horizontal neighbor has different fluid level
        std::array<glm::ivec3, 4> neighbors = {{
            {worldX - 1, worldY, worldZ}, {worldX + 1, worldY, worldZ},
            {worldX, worldY, worldZ - 1}, {worldX, worldY, worldZ + 1}
        }};

        for (const auto& pos : neighbors) {
            Game::BlockID neighbor = world.GetBlock(pos.x, pos.y, pos.z);
            if (neighbor != fluidType) {
                return true; // Flowing toward non-fluid
            }
        }

        return false;
    }

    float FluidMeshBuilder::GetDefaultFluidHeight(Game::BlockID fluidType) {
        switch (fluidType) {
            case Game::BlockID::Water:
                return config.waterLevel;
            case Game::BlockID::Lava:
                return config.lavaLevel;
            default:
                return 0.875f; // Default
        }
    }

    std::vector<glm::vec2> FluidMeshBuilder::GenerateFluidUVs(const AtlasUVRect& atlasUV, bool isTopFace) {
        std::vector<glm::vec2> uvs;
        uvs.reserve(4);

        // Standard quad UV mapping
        uvs.push_back(atlasUV.uvMin);                                    // Bottom-left
        uvs.push_back(glm::vec2(atlasUV.uvMax.x, atlasUV.uvMin.y));     // Bottom-right
        uvs.push_back(atlasUV.uvMax);                                    // Top-right
        uvs.push_back(glm::vec2(atlasUV.uvMin.x, atlasUV.uvMax.y));     // Top-left

        return uvs;
    }

    glm::vec4 FluidMeshBuilder::GetFluidColor(Game::BlockID fluidType) {
        switch (fluidType) {
            case Game::BlockID::Water:
                return glm::vec4(0.25f, 0.5f, 1.0f, 0.8f); // Blue with transparency
            case Game::BlockID::Lava:
                return glm::vec4(1.0f, 0.3f, 0.0f, 1.0f);  // Orange-red, opaque
            default:
                return glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);  // White
        }
    }

    void FluidMeshBuilder::AddFluidQuadVertices(const std::vector<glm::vec3>& positions,
                                               const glm::vec3& normal,
                                               const std::vector<glm::vec2>& uvs,
                                               const glm::vec4& color,
                                               LayerBuffers& targetLayer) {
        if (positions.size() != 4 || uvs.size() != 4) {
            Log::Error("Invalid fluid quad data: positions=%zu, uvs=%zu", positions.size(), uvs.size());
            return;
        }

        uint32_t baseIndex = targetLayer.verts.size();

        // Add 4 vertices
        for (int i = 0; i < 4; ++i) {
            Vertex vertex;
            vertex.pos = positions[i];
            vertex.nrm = normal;
            vertex.uv = uvs[i];
            vertex.color = color;
            vertex.ao = 255; // Full ambient occlusion for fluids

            targetLayer.verts.push_back(vertex);
        }

        // Add 6 indices for 2 triangles (quad)
        targetLayer.indices.push_back(baseIndex + 0);
        targetLayer.indices.push_back(baseIndex + 1);
        targetLayer.indices.push_back(baseIndex + 2);

        targetLayer.indices.push_back(baseIndex + 0);
        targetLayer.indices.push_back(baseIndex + 2);
        targetLayer.indices.push_back(baseIndex + 3);
    }

} // namespace Render