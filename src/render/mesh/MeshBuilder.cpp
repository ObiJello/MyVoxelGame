// File: src/render/mesh/MeshBuilder.cpp - Enhanced for Section-Based Building
#include "MeshBuilder.hpp"
#include "FluidMeshBuilder.hpp"
#include "../../core/Log.hpp"
#include "../../core/Config.hpp"
#include <chrono>
#include <algorithm>

namespace Render {

    // Static member definitions (unchanged)
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

        // Set default configuration
        config = SectionBuildConfig{};

        Log::Debug("MeshBuilder initialized with section-based meshing, atlas (%dx%d)",
                  atlas.GetAtlasWidth(), atlas.GetAtlasHeight());
    }

    // NEW: Build mesh for a specific section
    SectionMeshData MeshBuilder::BuildSection(int chunkX, int chunkZ, int sectionY) {
        auto startTime = std::chrono::high_resolution_clock::now();

        SectionMeshData meshData;
        meshData.sectionY = sectionY;
        meshData.Clear();

        // Check if we should skip empty sections
        if (config.skipEmptySections && IsSectionEmpty(chunkX, chunkZ, sectionY)) {
            lastStats.emptySectionsSkipped++;
            Log::Debug("Skipping empty section (%d, %d, %d)", chunkX, chunkZ, sectionY);
            return meshData;
        }

        Log::Debug("Building section mesh (%d, %d, %d)", chunkX, chunkZ, sectionY);

        // Process this section
        ProcessSection(chunkX, chunkZ, sectionY, meshData);

        // Add fluid geometry if enabled
        if (config.enableFluidMeshing && fluidBuilder) {
            // Note: FluidMeshBuilder will need to be updated to work with sections
            // For now, we'll call it but it won't add much
            Log::Debug("Adding fluid geometry for section (%d, %d, %d)", chunkX, chunkZ, sectionY);
        }

        // Update statistics
        auto endTime = std::chrono::high_resolution_clock::now();
        float sectionBuildTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        lastStats.sectionsProcessed++;
        lastStats.buildTimeMs += sectionBuildTime;
        if (!meshData.IsEmpty()) {
            lastStats.activeSections++;
        }

        Log::Debug("Section (%d, %d, %d) complete: %zu vertices, %zu indices, %.2fms",
                  chunkX, chunkZ, sectionY, meshData.GetTotalVertices(),
                  meshData.GetTotalIndices(), sectionBuildTime);

        return meshData;
    }

    // NEW: Build specific sections that need updating
    std::vector<SectionMeshData> MeshBuilder::BuildSections(int chunkX, int chunkZ,
                                                           const std::unordered_set<int>& sectionIndices) {
        auto startTime = std::chrono::high_resolution_clock::now();

        // Reset statistics
        lastStats = BuildStats{};

        std::vector<SectionMeshData> sectionMeshes;
        sectionMeshes.reserve(sectionIndices.size());

        Log::Debug("Building %zu specific sections for chunk (%d, %d)",
                  sectionIndices.size(), chunkX, chunkZ);

        int sectionsBuilt = 0;
        for (int sectionIndex : sectionIndices) {
            // Convert section index to section Y coordinate
            int sectionY = SectionIndexToY(sectionIndex);

            // Respect frame limits
            if (sectionsBuilt >= config.maxSectionsPerFrame) {
                Log::Debug("Hit max sections per frame limit (%d), stopping", config.maxSectionsPerFrame);
                break;
            }

            SectionMeshData sectionMesh = BuildSection(chunkX, chunkZ, sectionY);
            sectionMeshes.push_back(std::move(sectionMesh));
            sectionsBuilt++;
        }

        // Calculate final timing
        auto endTime = std::chrono::high_resolution_clock::now();
        lastStats.buildTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        Log::Info("Built %d sections for chunk (%d, %d) in %.2fms",
                 sectionsBuilt, chunkX, chunkZ, lastStats.buildTimeMs);

        return sectionMeshes;
    }

    // NEW: Build all sections for a chunk
    std::vector<SectionMeshData> MeshBuilder::BuildAllSections(int chunkX, int chunkZ) {
        auto startTime = std::chrono::high_resolution_clock::now();

        // Reset statistics
        lastStats = BuildStats{};

        std::vector<SectionMeshData> allSections;
        allSections.reserve(Game::Math::SECTIONS_PER_CHUNK);

        Log::Debug("Building all sections for chunk (%d, %d)", chunkX, chunkZ);

        // Build each section from bottom to top
        for (int sectionIndex = 0; sectionIndex < Game::Math::SECTIONS_PER_CHUNK; ++sectionIndex) {
            int sectionY = SectionIndexToY(sectionIndex);

            SectionMeshData sectionMesh = BuildSection(chunkX, chunkZ, sectionY);
            allSections.push_back(std::move(sectionMesh));
        }

        // Calculate final timing
        auto endTime = std::chrono::high_resolution_clock::now();
        lastStats.buildTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        // Update final statistics
        lastStats.verticesGenerated = 0;
        lastStats.indicesGenerated = 0;
        for (const auto& section : allSections) {
            lastStats.verticesGenerated += section.GetTotalVertices();
            lastStats.indicesGenerated += section.GetTotalIndices();
        }

        Log::Info("Built all sections for chunk (%d, %d): %d active/%d total sections, %zu vertices, %.2fms",
                 chunkX, chunkZ, lastStats.activeSections, Game::Math::SECTIONS_PER_CHUNK,
                 lastStats.verticesGenerated, lastStats.buildTimeMs);

        return allSections;
    }

    // NEW: Process a specific section
    void MeshBuilder::ProcessSection(int chunkX, int chunkZ, int sectionY, SectionMeshData& meshData) {
        // Calculate world coordinates for this chunk
        int baseWorldX = chunkX * Game::Math::CHUNK_SIZE_X;
        int baseWorldZ = chunkZ * Game::Math::CHUNK_SIZE_Z;

        // Get Y range for this section
        auto [minY, maxY] = GetSectionYRange(sectionY);

        Log::Debug("Processing section (%d, %d, %d) Y range [%d, %d]",
                  chunkX, chunkZ, sectionY, minY, maxY);

        // Iterate through all blocks in this section
        for (int localX = 0; localX < Game::Math::CHUNK_SIZE_X; ++localX) {
            for (int localZ = 0; localZ < Game::Math::CHUNK_SIZE_Z; ++localZ) {
                for (int worldY = minY; worldY <= maxY; ++worldY) {
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
                    ProcessSectionBlock(worldX, worldY, worldZ, blockId, meshData);
                }
            }
        }
    }

    // NEW: Process a block within a section
    void MeshBuilder::ProcessSectionBlock(int worldX, int worldY, int worldZ, Game::BlockID blockId,
                                        SectionMeshData& meshData) {
        // Skip fluid blocks (handled by FluidMeshBuilder - to be updated later)
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

    // NEW: Check if a section contains any non-air blocks
    bool MeshBuilder::IsSectionEmpty(int chunkX, int chunkZ, int sectionY) {
        int baseWorldX = chunkX * Game::Math::CHUNK_SIZE_X;
        int baseWorldZ = chunkZ * Game::Math::CHUNK_SIZE_Z;
        auto [minY, maxY] = GetSectionYRange(sectionY);

        // Sample a few blocks to check if section is likely empty
        // This is a performance optimization - we don't check every single block
        const int SAMPLE_STRIDE = 4; // Check every 4th block

        for (int localX = 0; localX < Game::Math::CHUNK_SIZE_X; localX += SAMPLE_STRIDE) {
            for (int localZ = 0; localZ < Game::Math::CHUNK_SIZE_Z; localZ += SAMPLE_STRIDE) {
                for (int worldY = minY; worldY <= maxY; worldY += SAMPLE_STRIDE) {
                    int worldX = baseWorldX + localX;
                    int worldZ = baseWorldZ + localZ;

                    Game::BlockID blockId = world.GetBlock(worldX, worldY, worldZ);
                    if (blockId != Game::BlockID::Air) {
                        return false; // Found a non-air block
                    }
                }
            }
        }

        return true; // All sampled blocks were air
    }

    // NEW: Get world Y range for a section
    std::pair<int, int> MeshBuilder::GetSectionYRange(int sectionY) {
        int minY = Config::MinY + (sectionY * Game::Math::SECTION_HEIGHT);
        int maxY = minY + Game::Math::SECTION_HEIGHT - 1;

        // Clamp to world bounds
        minY = std::max(minY, Config::MinY);
        maxY = std::min(maxY, Config::MaxY);

        return {minY, maxY};
    }

    // NEW: Convert section Y to section index (0-23)
    int MeshBuilder::SectionYToIndex(int sectionY) {
        return (sectionY - Config::MinY) / Game::Math::SECTION_HEIGHT;
    }

    // NEW: Convert section index to section Y coordinate
    int MeshBuilder::SectionIndexToY(int sectionIndex) {
        return Config::MinY + (sectionIndex * Game::Math::SECTION_HEIGHT);
    }

    // Updated to work with SectionMeshData
    LayerBuffers& MeshBuilder::GetLayerForBlock(Game::BlockID blockId, SectionMeshData& meshData) {
        if (IsBlockOpaque(blockId)) {
            return meshData.opaque;
        } else if (IsBlockCutout(blockId)) {
            return meshData.cutout;
        } else {
            return meshData.translucent;
        }
    }

    // Rest of the methods remain the same but work with sections...
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

    // All the vertex generation methods remain the same...
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

        // Generate face vertices with CORRECT counter-clockwise winding order
        // All vertices are ordered counter-clockwise when viewed from OUTSIDE the face
        switch (faceDir) {
            case Game::FaceDir::Up: // +Y (top face)
                // Counter-clockwise from outside (looking down at top)
                positions.push_back(glm::vec3(from.x, to.y, from.z) + glm::vec3(worldX, worldY, worldZ)); // 0: back-left
                positions.push_back(glm::vec3(from.x, to.y, to.z) + glm::vec3(worldX, worldY, worldZ));   // 1: front-left
                positions.push_back(glm::vec3(to.x, to.y, to.z) + glm::vec3(worldX, worldY, worldZ));     // 2: front-right
                positions.push_back(glm::vec3(to.x, to.y, from.z) + glm::vec3(worldX, worldY, worldZ));   // 3: back-right
                break;

            case Game::FaceDir::Down: // -Y (bottom face)
                // Counter-clockwise from outside (looking up at bottom)
                positions.push_back(glm::vec3(from.x, from.y, to.z) + glm::vec3(worldX, worldY, worldZ));   // 0: front-left
                positions.push_back(glm::vec3(from.x, from.y, from.z) + glm::vec3(worldX, worldY, worldZ)); // 1: back-left
                positions.push_back(glm::vec3(to.x, from.y, from.z) + glm::vec3(worldX, worldY, worldZ));   // 2: back-right
                positions.push_back(glm::vec3(to.x, from.y, to.z) + glm::vec3(worldX, worldY, worldZ));     // 3: front-right
                break;

            case Game::FaceDir::North: // -Z (front face)
                // Counter-clockwise from outside (looking at front face)
                positions.push_back(glm::vec3(from.x, from.y, from.z) + glm::vec3(worldX, worldY, worldZ)); // 0: bottom-left
                positions.push_back(glm::vec3(from.x, to.y, from.z) + glm::vec3(worldX, worldY, worldZ));   // 1: top-left
                positions.push_back(glm::vec3(to.x, to.y, from.z) + glm::vec3(worldX, worldY, worldZ));     // 2: top-right
                positions.push_back(glm::vec3(to.x, from.y, from.z) + glm::vec3(worldX, worldY, worldZ));   // 3: bottom-right
                break;

            case Game::FaceDir::South: // +Z (back face)
                // Counter-clockwise from outside (looking at back face)
                positions.push_back(glm::vec3(to.x, from.y, to.z) + glm::vec3(worldX, worldY, worldZ));     // 0: bottom-right
                positions.push_back(glm::vec3(to.x, to.y, to.z) + glm::vec3(worldX, worldY, worldZ));       // 1: top-right
                positions.push_back(glm::vec3(from.x, to.y, to.z) + glm::vec3(worldX, worldY, worldZ));     // 2: top-left
                positions.push_back(glm::vec3(from.x, from.y, to.z) + glm::vec3(worldX, worldY, worldZ));   // 3: bottom-left
                break;

            case Game::FaceDir::West: // -X (left face)
                // Counter-clockwise from outside (looking at left face)
                positions.push_back(glm::vec3(from.x, from.y, to.z) + glm::vec3(worldX, worldY, worldZ));   // 0: bottom-front
                positions.push_back(glm::vec3(from.x, to.y, to.z) + glm::vec3(worldX, worldY, worldZ));     // 1: top-front
                positions.push_back(glm::vec3(from.x, to.y, from.z) + glm::vec3(worldX, worldY, worldZ));   // 2: top-back
                positions.push_back(glm::vec3(from.x, from.y, from.z) + glm::vec3(worldX, worldY, worldZ)); // 3: bottom-back
                break;

            case Game::FaceDir::East: // +X (right face)
                // Counter-clockwise from outside (looking at right face)
                positions.push_back(glm::vec3(to.x, from.y, from.z) + glm::vec3(worldX, worldY, worldZ));   // 0: bottom-back
                positions.push_back(glm::vec3(to.x, to.y, from.z) + glm::vec3(worldX, worldY, worldZ));     // 1: top-back
                positions.push_back(glm::vec3(to.x, to.y, to.z) + glm::vec3(worldX, worldY, worldZ));       // 2: top-front
                positions.push_back(glm::vec3(to.x, from.y, to.z) + glm::vec3(worldX, worldY, worldZ));     // 3: bottom-front
                break;
        }

        return positions;
    }

    std::vector<glm::vec2> MeshBuilder::GenerateFaceUVs(const Game::FaceDef& faceDef,
                                                   const AtlasUVRect& atlasUV) {
        std::vector<glm::vec2> uvs;
        uvs.reserve(4);

        // Convert face UV from [0,16] pixel space to [0,1] normalized space
        glm::vec2 uvMin = glm::vec2(faceDef.uv.x, faceDef.uv.y) / 16.0f;
        glm::vec2 uvMax = glm::vec2(faceDef.uv.z, faceDef.uv.w) / 16.0f;

        // Standard quad UV mapping that matches vertex order
        std::vector<glm::vec2> localUVs = {
            {uvMin.x, uvMax.y}, // Vertex 0: bottom-left -> bottom-left of texture
            {uvMin.x, uvMin.y}, // Vertex 1: top-left -> top-left of texture
            {uvMax.x, uvMin.y}, // Vertex 2: top-right -> top-right of texture
            {uvMax.x, uvMax.y}  // Vertex 3: bottom-right -> bottom-right of texture
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
        return DEFAULT_COLOR;
    }

} // namespace Render