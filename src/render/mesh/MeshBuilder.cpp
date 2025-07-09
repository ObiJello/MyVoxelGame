// File: src/render/mesh/MeshBuilder.cpp
#include "MeshBuilder.hpp"
#include "FluidMeshBuilder.hpp"
#include "../../core/Log.hpp"
#include "../../core/Config.hpp"
#include <chrono>
#include <algorithm>

namespace Render {

    // Static member definitions
    const std::array<glm::ivec3, 6> MeshBuilder::FACE_OFFSETS = {{
        {0, 1, 0},   // Up (+Y)
        {0, -1, 0},  // Down (-Y)
        {0, 0, -1},  // North (-Z)
        {0, 0, 1},   // South (+Z)
        {-1, 0, 0},  // West (-X)
        {1, 0, 0}    // East (+X)
    }};

    const std::array<glm::vec3, 6> MeshBuilder::FACE_NORMALS = {{
        {0.0f, 1.0f, 0.0f},   // Up
        {0.0f, -1.0f, 0.0f},  // Down
        {0.0f, 0.0f, -1.0f},  // North
        {0.0f, 0.0f, 1.0f},   // South
        {-1.0f, 0.0f, 0.0f},  // West
        {1.0f, 0.0f, 0.0f}    // East
    }};

    MeshBuilder::MeshBuilder(Game::World& world, AtlasBuilder& atlas)
        : world(world), atlas(atlas) {
        // Create fluid mesh builder for specialized water/lava rendering
        fluidBuilder = std::make_unique<FluidMeshBuilder>(world, atlas);

        Log::Debug("MeshBuilder initialized with atlas (%dx%d)",
                  atlas.GetAtlasWidth(), atlas.GetAtlasHeight());
    }

    ChunkMeshData MeshBuilder::Build(int chunkX, int chunkZ) {
        auto startTime = std::chrono::high_resolution_clock::now();

        // Reset statistics
        lastStats = BuildStats{};

        ChunkMeshData meshData;
        meshData.Clear();

        Log::Debug("Building mesh for chunk (%d, %d)", chunkX, chunkZ);

        // Process all solid and cutout blocks
        ProcessChunk(chunkX, chunkZ, meshData);

        // Add fluid geometry using specialized builder
        if (fluidBuilder) {
            fluidBuilder->AppendFluidQuads(chunkX, chunkZ, meshData);
        }

        // Calculate timing
        auto endTime = std::chrono::high_resolution_clock::now();
        lastStats.buildTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        // Update final statistics
        lastStats.verticesGenerated = meshData.GetTotalVertices();
        lastStats.indicesGenerated = meshData.GetTotalIndices();
        lastStats.opaqueVertices = meshData.opaque.GetVertexCount();
        lastStats.cutoutVertices = meshData.cutout.GetVertexCount();
        lastStats.translucentVertices = meshData.translucent.GetVertexCount();

        Log::Debug("Chunk (%d, %d) mesh complete: %zu vertices, %zu indices, %.2fms",
                  chunkX, chunkZ, meshData.GetTotalVertices(),
                  meshData.GetTotalIndices(), lastStats.buildTimeMs);

        return meshData;
    }

    void MeshBuilder::ProcessChunk(int chunkX, int chunkZ, ChunkMeshData& meshData) {
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

                    // Skip air blocks immediately
                    if (blockId == Game::BlockID::Air) {
                        continue;
                    }

                    lastStats.blocksProcessed++;

                    // Process non-air blocks
                    ProcessBlock(worldX, worldY, worldZ, blockId, meshData);
                }
            }
        }
    }

    void MeshBuilder::ProcessBlock(int worldX, int worldY, int worldZ,
                                  Game::BlockID blockId, ChunkMeshData& meshData) {
        // Skip fluid blocks (handled by FluidMeshBuilder)
        if (IsBlockFluid(blockId)) {
            return;
        }

        // Get the block model
        const Game::BlockModel& model = Game::BlockRegistry::GetBlockModel(blockId);

        // Determine which render layer this block belongs to
        LayerBuffers& targetLayer = GetLayerForBlock(blockId, meshData);

        // Generate faces for all elements in the model
        GenerateBlockFaces(worldX, worldY, worldZ, model, targetLayer);
    }

    LayerBuffers& MeshBuilder::GetLayerForBlock(Game::BlockID blockId, ChunkMeshData& meshData) {
        if (IsBlockOpaque(blockId)) {
            return meshData.opaque;
        } else if (IsBlockCutout(blockId)) {
            return meshData.cutout;
        } else {
            return meshData.translucent;
        }
    }

    bool MeshBuilder::IsBlockOpaque(Game::BlockID blockId) {
        const Game::Block& block = Game::BlockRegistry::Get(blockId);
        return block.opaque;
    }

    bool MeshBuilder::IsBlockCutout(Game::BlockID blockId) {
        // Blocks that use alpha testing (leaves, grass, etc.)
        switch (blockId) {
            case Game::BlockID::Leaves:
            case Game::BlockID::CherryLeaves:
                return true;
            default:
                return false;
        }
    }

    bool MeshBuilder::IsBlockTranslucent(Game::BlockID blockId) {
        const Game::Block& block = Game::BlockRegistry::Get(blockId);

        // If not opaque and not cutout, it's translucent
        if (!block.opaque && !IsBlockCutout(blockId) && !IsBlockFluid(blockId)) {
            return true;
        }

        return false;
    }

    bool MeshBuilder::IsBlockFluid(Game::BlockID blockId) {
        return blockId == Game::BlockID::Water || blockId == Game::BlockID::Lava;
    }

    void MeshBuilder::GenerateBlockFaces(int worldX, int worldY, int worldZ,
                                        const Game::BlockModel& model,
                                        LayerBuffers& targetLayer) {
        // Process each element in the block model
        for (const auto& element : model.elements) {
            // Check each face of the element
            for (const auto& [faceDir, faceDef] : element.faces) {
                // Perform face culling
                if (ShouldRenderFace(worldX, worldY, worldZ, faceDir,
                                   world.GetBlock(worldX, worldY, worldZ))) {
                    GenerateFace(element, faceDir, faceDef, worldX, worldY, worldZ, targetLayer);
                    lastStats.facesGenerated++;
                }
            }
        }
    }

    bool MeshBuilder::ShouldRenderFace(int x, int y, int z, Game::FaceDir faceDir,
                                      Game::BlockID currentBlock) {
        // Get neighbor block in the direction of this face
        Game::BlockID neighbor = GetNeighborBlock(x, y, z, faceDir);

        // Always render if neighbor is air
        if (neighbor == Game::BlockID::Air) {
            return true;
        }

        // Don't render if neighbor is opaque (face culling)
        if (IsBlockOpaque(neighbor)) {
            return false;
        }

        // For translucent blocks, render face if neighbor is different block type
        if (IsBlockTranslucent(currentBlock)) {
            return neighbor != currentBlock;
        }

        // Default: render the face
        return true;
    }

    Game::BlockID MeshBuilder::GetNeighborBlock(int x, int y, int z, Game::FaceDir faceDir) {
        glm::ivec3 offset = GetFaceOffset(faceDir);
        return world.GetBlock(x + offset.x, y + offset.y, z + offset.z);
    }

    glm::ivec3 MeshBuilder::GetFaceOffset(Game::FaceDir faceDir) {
        return FACE_OFFSETS[static_cast<int>(faceDir)];
    }

    void MeshBuilder::GenerateFace(const Game::Element& element, Game::FaceDir faceDir,
                                  const Game::FaceDef& faceDef, int worldX, int worldY, int worldZ,
                                  LayerBuffers& targetLayer) {
        // Generate the 4 corner positions for this face
        std::vector<glm::vec3> positions = GenerateFacePositions(element, faceDir, worldX, worldY, worldZ);

        // Get face normal
        glm::vec3 normal = GetFaceNormal(faceDir);

        // Resolve texture and get atlas UV coordinates
        const Game::BlockModel& model = Game::BlockRegistry::GetBlockModel(
            world.GetBlock(worldX, worldY, worldZ));
        std::string texturePath = model.ResolveTexture(faceDef.textureRef);

        AtlasUVRect atlasUV;
        if (!GetTextureUV(texturePath, atlasUV)) {
            Log::Warning("Failed to get UV for texture: %s", texturePath.c_str());
            // Use a default/error texture
            texturePath = "missingno";
            GetTextureUV(texturePath, atlasUV);
        }

        // Generate UV coordinates for this face
        std::vector<glm::vec2> uvs = GenerateFaceUVs(faceDef, atlasUV);

        // Calculate biome tinting
        glm::vec4 color = CalculateBiomeTint(worldX, worldY, worldZ, faceDef,
                                           world.GetBlock(worldX, worldY, worldZ));

        // Add vertices to the target layer
        AddQuadVertices(positions, normal, uvs, color, targetLayer);
    }

    std::vector<glm::vec3> MeshBuilder::GenerateFacePositions(const Game::Element& element,
                                                             Game::FaceDir faceDir,
                                                             int worldX, int worldY, int worldZ) {
        std::vector<glm::vec3> positions;
        positions.reserve(4);

        // Convert element coordinates from [0,16] to [0,1] space
        glm::vec3 from = element.from / 16.0f;
        glm::vec3 to = element.to / 16.0f;

        // Generate face vertices based on direction
        switch (faceDir) {
            case Game::FaceDir::Up: // +Y (top)
                positions.push_back(glm::vec3(from.x, to.y, from.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(to.x, to.y, from.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(to.x, to.y, to.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(from.x, to.y, to.z) + glm::vec3(worldX, worldY, worldZ));
                break;

            case Game::FaceDir::Down: // -Y (bottom)
                positions.push_back(glm::vec3(from.x, from.y, to.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(to.x, from.y, to.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(to.x, from.y, from.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(from.x, from.y, from.z) + glm::vec3(worldX, worldY, worldZ));
                break;

            case Game::FaceDir::North: // -Z (front)
                positions.push_back(glm::vec3(to.x, from.y, from.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(from.x, from.y, from.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(from.x, to.y, from.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(to.x, to.y, from.z) + glm::vec3(worldX, worldY, worldZ));
                break;

            case Game::FaceDir::South: // +Z (back)
                positions.push_back(glm::vec3(from.x, from.y, to.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(to.x, from.y, to.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(to.x, to.y, to.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(from.x, to.y, to.z) + glm::vec3(worldX, worldY, worldZ));
                break;

            case Game::FaceDir::West: // -X (left)
                positions.push_back(glm::vec3(from.x, from.y, from.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(from.x, from.y, to.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(from.x, to.y, to.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(from.x, to.y, from.z) + glm::vec3(worldX, worldY, worldZ));
                break;

            case Game::FaceDir::East: // +X (right)
                positions.push_back(glm::vec3(to.x, from.y, to.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(to.x, from.y, from.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(to.x, to.y, from.z) + glm::vec3(worldX, worldY, worldZ));
                positions.push_back(glm::vec3(to.x, to.y, to.z) + glm::vec3(worldX, worldY, worldZ));
                break;
        }

        return positions;
    }

    std::vector<glm::vec2> MeshBuilder::GenerateFaceUVs(const Game::FaceDef& faceDef,
                                                       const AtlasUVRect& atlasUV) {
        std::vector<glm::vec2> uvs;
        uvs.reserve(4);

        // Convert face UV from [0,16] pixel space to [0,1] normalized space
        glm::vec2 uvMin = faceDef.uv / 16.0f;
        glm::vec2 uvMax = glm::vec2(faceDef.uv.z, faceDef.uv.w) / 16.0f;

        // Standard quad UV mapping (0,0 is top-left in texture space)
        std::vector<glm::vec2> localUVs = {
            {uvMin.x, uvMin.y}, // Bottom-left
            {uvMax.x, uvMin.y}, // Bottom-right
            {uvMax.x, uvMax.y}, // Top-right
            {uvMin.x, uvMax.y}  // Top-left
        };

        // Interpolate to atlas coordinates
        for (const auto& localUV : localUVs) {
            uvs.push_back(InterpolateUV(atlasUV.uvMin, atlasUV.uvMax, localUV));
        }

        return uvs;
    }

    glm::vec3 MeshBuilder::GetFaceNormal(Game::FaceDir faceDir) {
        return FACE_NORMALS[static_cast<int>(faceDir)];
    }

    void MeshBuilder::AddQuadVertices(const std::vector<glm::vec3>& positions,
                                     const glm::vec3& normal,
                                     const std::vector<glm::vec2>& uvs,
                                     const glm::vec4& color,
                                     LayerBuffers& targetLayer) {
        if (positions.size() != 4 || uvs.size() != 4) {
            Log::Error("Invalid quad data: positions=%zu, uvs=%zu", positions.size(), uvs.size());
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
            vertex.ao = 255; // Full ambient occlusion for now

            targetLayer.verts.push_back(vertex);
        }

        // Add 6 indices for 2 triangles (quad)
        // Triangle 1: 0-1-2
        targetLayer.indices.push_back(baseIndex + 0);
        targetLayer.indices.push_back(baseIndex + 1);
        targetLayer.indices.push_back(baseIndex + 2);

        // Triangle 2: 0-2-3
        targetLayer.indices.push_back(baseIndex + 0);
        targetLayer.indices.push_back(baseIndex + 2);
        targetLayer.indices.push_back(baseIndex + 3);
    }

    bool MeshBuilder::GetTextureUV(const std::string& texturePath, AtlasUVRect& uvRect) {
        return atlas.GetUVRect(texturePath, uvRect);
    }

    glm::vec2 MeshBuilder::InterpolateUV(const glm::vec2& uvMin, const glm::vec2& uvMax,
                                        const glm::vec2& localUV) {
        return glm::mix(uvMin, uvMax, localUV);
    }

    glm::vec4 MeshBuilder::CalculateBiomeTint(int worldX, int worldY, int worldZ,
                                             const Game::FaceDef& faceDef,
                                             Game::BlockID blockId) {
        // If this face doesn't use biome tinting, return white
        if (faceDef.tintIndex < 0) {
            return DEFAULT_COLOR;
        }

        // For now, return default white color
        // TODO: Implement proper biome tinting based on grass/foliage colormaps
        // This would involve:
        // 1. Getting biome at world position
        // 2. Sampling appropriate colormap texture
        // 3. Returning the tinted color

        return DEFAULT_COLOR;
    }

} // namespace Render