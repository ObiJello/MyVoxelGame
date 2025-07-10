// File: src/render/mesh/MeshBuilder.hpp - Enhanced for Section-Based Building
#pragma once

#include "../Vertex.hpp"
#include "../atlas/AtlasBuilder.hpp"
#include "../../engine/world/World.hpp"
#include "../../engine/block/BlockRegistry.hpp"
#include "../../engine/block/BlockModel.hpp"
#include "../../game/WorldMath.hpp"
#include <vector>
#include <cstdint>
#include <memory>
#include <unordered_set>

namespace Render {

    // Forward declarations
    class FluidMeshBuilder;

    // Layer-specific buffers for the three render passes
    struct LayerBuffers {
        std::vector<Vertex> verts;
        std::vector<uint32_t> indices;

        void Clear() {
            verts.clear();
            indices.clear();
        }

        size_t GetVertexCount() const { return verts.size(); }
        size_t GetIndexCount() const { return indices.size(); }

        bool IsEmpty() const { return verts.empty(); }
    };

    // Section mesh data before GPU upload (CPU-side buffers)
    struct SectionMeshData {
        LayerBuffers opaque;      // Solid blocks layer
        LayerBuffers cutout;      // Alpha-test blocks layer
        LayerBuffers translucent; // Blended blocks layer

        int sectionY = 0;         // Section Y coordinate

        void Clear() {
            opaque.Clear();
            cutout.Clear();
            translucent.Clear();
        }

        size_t GetTotalVertices() const {
            return opaque.GetVertexCount() + cutout.GetVertexCount() + translucent.GetVertexCount();
        }

        size_t GetTotalIndices() const {
            return opaque.GetIndexCount() + cutout.GetIndexCount() + translucent.GetIndexCount();
        }

        bool IsEmpty() const {
            return opaque.IsEmpty() && cutout.IsEmpty() && translucent.IsEmpty();
        }
    };

    class MeshBuilder {
    public:
        MeshBuilder(Game::World& world, AtlasBuilder& atlas);
        ~MeshBuilder() = default;

        // NEW: Build mesh for a specific section of a chunk
        SectionMeshData BuildSection(int chunkX, int chunkZ, int sectionY);

        // NEW: Build mesh for specific sections that need updating
        std::vector<SectionMeshData> BuildSections(int chunkX, int chunkZ,
                                                   const std::unordered_set<int>& sectionIndices);

        // NEW: Build all sections for a chunk (returns up to 24 sections)
        std::vector<SectionMeshData> BuildAllSections(int chunkX, int chunkZ);

        // Get statistics from last build operation
        struct BuildStats {
            int sectionsProcessed = 0;
            int blocksProcessed = 0;
            int facesGenerated = 0;
            int verticesGenerated = 0;
            int indicesGenerated = 0;
            float buildTimeMs = 0.0f;

            // Per-layer breakdown
            int opaqueVertices = 0;
            int cutoutVertices = 0;
            int translucentVertices = 0;

            // Section-specific stats
            int emptySectionsSkipped = 0;
            int activeSections = 0;
        };

        const BuildStats& GetLastBuildStats() const { return lastStats; }

        // Configuration for section building
        struct SectionBuildConfig {
            bool skipEmptySections = true;      // Skip sections with no blocks
            bool includeNeighborData = true;    // Include neighbor blocks for face culling
            int maxSectionsPerFrame = 4;        // Limit sections built per frame
            bool enableFluidMeshing = true;     // Include fluid geometry
        };

        void SetConfig(const SectionBuildConfig& config) { this->config = config; }
        const SectionBuildConfig& GetConfig() const { return config; }

    private:
        Game::World& world;
        AtlasBuilder& atlas;
        BuildStats lastStats;
        SectionBuildConfig config;

        // Fluid mesh builder for specialized water/lava rendering
        std::unique_ptr<FluidMeshBuilder> fluidBuilder;

        // NEW: Section-based mesh building methods
        void ProcessSection(int chunkX, int chunkZ, int sectionY, SectionMeshData& meshData);
        void ProcessSectionBlock(int worldX, int worldY, int worldZ, Game::BlockID blockId,
                               SectionMeshData& meshData);

        // NEW: Check if a section contains any non-air blocks
        bool IsSectionEmpty(int chunkX, int chunkZ, int sectionY);

        // NEW: Get world Y range for a section
        std::pair<int, int> GetSectionYRange(int sectionY);

        // NEW: Convert section Y to section index (0-23)
        int SectionYToIndex(int sectionY);

        // NEW: Convert section index to section Y coordinate
        int SectionIndexToY(int sectionIndex);

        // Block classification for render layers
        LayerBuffers& GetLayerForBlock(Game::BlockID blockId, SectionMeshData& meshData);
        bool IsBlockOpaque(Game::BlockID blockId);
        bool IsBlockCutout(Game::BlockID blockId);
        bool IsBlockTranslucent(Game::BlockID blockId);
        bool IsBlockFluid(Game::BlockID blockId);

        // Face culling and visibility
        bool ShouldRenderFace(int x, int y, int z, Game::FaceDir faceDir, Game::BlockID currentBlock);
        Game::BlockID GetNeighborBlock(int x, int y, int z, Game::FaceDir faceDir);
        glm::ivec3 GetFaceOffset(Game::FaceDir faceDir);

        // Vertex generation from block models
        void GenerateBlockFaces(int worldX, int worldY, int worldZ,
                               const Game::BlockModel& model,
                               LayerBuffers& targetLayer);

        void GenerateFace(const Game::Element& element, Game::FaceDir faceDir,
                         const Game::FaceDef& faceDef, int worldX, int worldY, int worldZ,
                         LayerBuffers& targetLayer);

        // UV mapping and texture atlas integration
        bool GetTextureUV(const std::string& texturePath, AtlasUVRect& uvRect);
        glm::vec2 InterpolateUV(const glm::vec2& uvMin, const glm::vec2& uvMax, const glm::vec2& localUV);

        // Face normal calculation
        glm::vec3 GetFaceNormal(Game::FaceDir faceDir);

        // Vertex generation helpers
        void AddQuadVertices(const std::vector<glm::vec3>& positions,
                           const glm::vec3& normal,
                           const std::vector<glm::vec2>& uvs,
                           const glm::vec4& color,
                           LayerBuffers& targetLayer);

        // Generate the 4 corner positions for a face
        std::vector<glm::vec3> GenerateFacePositions(const Game::Element& element,
                                                    Game::FaceDir faceDir,
                                                    int worldX, int worldY, int worldZ);

        // Generate UV coordinates for a face
        std::vector<glm::vec2> GenerateFaceUVs(const Game::FaceDef& faceDef,
                                              const AtlasUVRect& atlasUV);

        // Biome tinting support
        glm::vec4 CalculateBiomeTint(int worldX, int worldY, int worldZ,
                                   const Game::FaceDef& faceDef,
                                   Game::BlockID blockId);

        // Default white color for non-tinted blocks
        static constexpr glm::vec4 DEFAULT_COLOR = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);

        // Face direction to offset mapping
        static const std::array<glm::ivec3, 6> FACE_OFFSETS;
        static const std::array<glm::vec3, 6> FACE_NORMALS;
    };

} // namespace Render