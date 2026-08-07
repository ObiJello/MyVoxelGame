// File: src/client/renderer/mesh/Mesher.hpp
#pragma once

#include "SectionMesh.hpp"
#include "FluidMeshBuilder.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/block/BlockModel.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "../texture/AtlasBuilder.hpp"
#include <glm/glm.hpp>
#include <array>
#include <memory>

// **NEW**: Forward declaration to avoid circular dependency
namespace Game {
    class World;
}

namespace Client {
namespace Render {
    struct SectionSnapshot;  // MeshJobData.hpp — fast-path mesh input
}
}

namespace Render {

    // Face directions for block meshing
    enum class BlockFace : int {
        PositiveY = 0,  // Top (+Y)
        NegativeY = 1,  // Bottom (-Y)
        PositiveZ = 2,  // Front (+Z)
        NegativeZ = 3,  // Back (-Z)
        PositiveX = 4,  // Right (+X)
        NegativeX = 5   // Left (-X)
    };

    // Mesh generation configuration
    struct MeshConfig {
        bool enableAmbientOcclusion = true;
        bool enableFaceCulling = true;
        bool enableBiomeTinting = true;
        float biomeTintStrength = 1.0f;

        // Performance settings
        bool enableGreedyMeshing = false;  // Future optimization
        int maxQuadsPerSection = 16384;    // Safety limit
    };

    // Render layer classification
    enum class RenderLayer {
        Opaque,      // Solid blocks (stone, dirt, wood)
        Cutout,      // Alpha-test blocks (leaves, grass, flowers)
        Translucent  // Blended blocks (glass, water, ice)
    };

    // Helper functions for render layer classification
    RenderLayer ClassifyBlock(Game::BlockID blockId);
    bool IsBlockOpaque(Game::BlockID blockId);
    bool IsBlockTranslucent(Game::BlockID blockId);

    // Core meshing class - turns block data into renderable geometry
    class Mesher {
    public:
        explicit Mesher(const MeshConfig& config = MeshConfig{});

        // **NEW**: Set world reference for cross-chunk neighbor access
        void SetWorld(Game::World* world);

        // Rebuild mesh for one 16x16x16 section (generic path: fills the block
        // cache through the IBlockAccess interface, then meshes from the cache)
        void BuildSectionMesh(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos, int sectionY, SectionMesh& outMesh);

        // Fast path for worker threads: fills the block cache directly from the
        // snapshot's flat arrays (memcpy for the interior and axis-aligned halo
        // planes) instead of ~10k virtual GetBlock calls per section. Produces
        // identical output to the IBlockAccess path over a SnapshotBlockAccess.
        void BuildSectionMesh(const Client::Render::SectionSnapshot& snapshot,
                              Game::Math::ChunkPos chunkPos, int sectionY, SectionMesh& outMesh);

        // Convenience: rebuild entire chunk (all 24 sections)
        // DEPRECATED: Use BuildSectionMesh with IBlockAccess instead
        // void BuildChunkMesh(const Game::Chunk& chunk, ChunkMesh& outMesh);

        // Update configuration
        void SetConfig(const MeshConfig& config) { m_config = config; }
        const MeshConfig& GetConfig() const { return m_config; }

        // Get statistics from last mesh operation
        struct MeshStats {
            int facesGenerated = 0;
            int facesCulled = 0;
            int quadsGenerated = 0;
            float buildTimeMs = 0.0f;
        };
        const MeshStats& GetLastStats() const { return m_lastStats; }

        // Block property cache: avoids per-block registry lookups during meshing.
        // Populated once per thread from the static BlockRegistry, then reused for all
        // subsequent mesh builds on that thread. Public so free functions
        // (ClassifyBlock, IsBlockOpaque) in the same namespace can access them.
        struct CachedBlockProps {
            bool isOpaque;
            RenderLayer renderLayer;
        };
        static constexpr size_t BLOCK_ID_COUNT = static_cast<size_t>(Game::BlockID::Count);
        static thread_local std::array<CachedBlockProps, BLOCK_ID_COUNT> s_blockPropsCache;
        static thread_local bool s_blockPropsCacheValid;

    private:
        MeshConfig m_config;
        mutable MeshStats m_lastStats;
        Game::World* m_world;  // World reference for cross-chunk access
        std::unique_ptr<FluidMeshBuilder> m_fluidBuilder;  // Fluid mesh builder

        void EnsureBlockPropsCache();

        // Per-section block/opaque caches: 18x18x18 covering the 16x16x16 section
        // plus a 1-block border on all sides. Built once at the start of
        // BuildSectionMesh; ALL subsequent block reads (main loop, face culling,
        // AO, fluid neighbor sampling via CacheBlockAccess) index these arrays —
        // no virtual GetBlock calls remain on the meshing hot path.
        // Layout is [y][z][x] (x contiguous) to match SectionSnapshot's flat
        // array so the interior and Y/Z halo planes fill via memcpy.
        // Index with [localY+1][localZ+1][localX+1] where local coords are in [-1,16].
        Game::BlockID m_blockCache[18][18][18];
        bool m_opaqueCache[18][18][18];
        int m_sectionBaseWorldX;
        int m_sectionBaseWorldY;
        int m_sectionBaseWorldZ;
        void FillBlockCacheFromAccess(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos, int sectionY);
        void FillBlockCacheFromSnapshot(const Client::Render::SectionSnapshot& snapshot,
                                        Game::Math::ChunkPos chunkPos, int sectionY);
        void DeriveOpaqueCache();
        // Shared meshing body — reads only from m_blockCache/m_opaqueCache
        void BuildSectionMeshFromCache(Game::Math::ChunkPos chunkPos, int sectionY, SectionMesh& outMesh);
        bool GetCachedOpaque(int worldX, int worldY, int worldZ) const;
        Game::BlockID GetCachedBlock(int worldX, int worldY, int worldZ) const;

        // UV cache: thread-local so it persists across Mesher instances on the same
        // worker thread, avoiding ResolveTexture string allocs + atlas hash lookups
        // on every mesh rebuild.
        static thread_local std::unordered_map<const Game::FaceDef*, glm::vec4> s_faceUVCache;

        // Core meshing functions
        void ProcessBlock(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos,
                         int localX, int localY, int localZ,
                         int sectionY, Game::BlockID blockId, SectionMesh& mesh);

        void AddBlockFace(const Game::IBlockAccess& blocks,
                         const Game::BlockModel& model, const Game::Element& element,
                         Game::FaceDir faceDir, const Game::FaceDef& faceDef,
                         glm::vec3 blockPos, glm::vec3 faceNormal, Game::BlockID blockId,
                         int worldX, int worldY, int worldZ, RenderLayer layer, SectionMesh& mesh);

        void GenerateQuad(const std::array<Vertex, 4>& quadVerts,
                         std::vector<Vertex>& outVerts, std::vector<uint16_t>& outIndices);

        // Culling and optimization (uses m_opaqueCache for fast neighbor lookups)
        bool ShouldCullFace(int worldX, int worldY, int worldZ, BlockFace face);

        // Cross-chunk neighbor lookup via IBlockAccess
        Game::BlockID GetNeighborBlock(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ,
                                      BlockFace face);

        // Texture and material helpers
        bool GetTextureUV(const std::string& texturePath, glm::vec4& uvRect);

        // **NEW**: Biome tinting methods for different tint indices
        glm::vec4 CalculateGrassTint(Game::BlockID blockId, int worldX, int worldY, int worldZ);
        glm::vec4 CalculateFoliageTint(Game::BlockID blockId, int worldX, int worldY, int worldZ);
        glm::vec4 CalculateBiomeTint(Game::BlockID blockId, int worldX, int worldY, int worldZ);

        // Geometry helpers. `elemMin`/`elemMax` are the element's bounds in
        // [0,1] block-space (already divided by 16 from MC's pixel-space
        // from/to). `faceUv` is the element-face's `uv` field in MC pixel
        // units [0,16] — it selects a sub-rect of the atlas sprite `uvRect`.
        // Full-cube blocks pass elemMin=(0,0,0), elemMax=(1,1,1), faceUv=
        // (0,0,16,16) and behave exactly as before.
        std::array<Vertex, 4> CreateFaceVertices(glm::vec3 blockPos, BlockFace face,
                                              const glm::vec4& uvRect, const glm::vec4& tint,
                                              const glm::vec3& elemMin, const glm::vec3& elemMax,
                                              const glm::vec4& faceUv);
        glm::vec3 GetFaceNormal(BlockFace face);

        // Minecraft-style per-vertex ambient occlusion
        // Returns a shade value 0.0-1.0 for a vertex corner based on 3 neighbor blocks
        float CalculateVertexAO(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ,
                                BlockFace face, int vertexIndex);

        // Minecraft directional face shading multiplier
        static float GetDirectionalShade(BlockFace face);

        // **REMOVED**: WorldYToChunkY() - use Game::Math::WorldCoordinates instead

        // **UPDATED**: Use WorldCoordinates for coordinate conversion
        glm::vec3 LocalToWorldPos(const Game::Math::ChunkPos& chunkPos, int localX, int worldY, int localZ) const {
            return glm::vec3(
                chunkPos.x * Game::Math::CHUNK_SIZE_X + localX,
                static_cast<float>(worldY),
                chunkPos.z * Game::Math::CHUNK_SIZE_Z + localZ
            );
        }
    };

} // namespace Render