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
    struct RegionSnapshot;   // MeshJobData.hpp — the 3x3x3 mesh input
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
        void BuildSectionMesh(const Client::Render::RegionSnapshot& region,
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
            // Whether this block declares any blockstate properties. Lets
            // ProcessBlock skip the per-voxel state lookup entirely for the
            // ~99% of blocks that have none.
            bool hasStates;

            // MC BlockColors.createDefault, flattened. Vanilla dispatches the
            // tint on the BLOCK and treats tintIndex only as a filter inside
            // that block's resolver — which is why grass_block (tintindex 0)
            // takes the GRASS colormap while oak_leaves (also tintindex 0)
            // takes FOLIAGE. Dispatching on the index alone, as this mesher
            // used to, cannot express that distinction at all.
            enum class TintSource : uint8_t {
                None,      // no resolver registered -> untinted (MC returns -1)
                Biome,     // blend `tintChannel` over the biome grid
                Constant,  // fixed colour (spruce / birch leaves)
                FlowerBed, // tintIndex 0 untinted, otherwise grass
                StemAge,   // melon / pumpkin stem: colour computed from `age`
            };
            TintSource tintSource = TintSource::None;
            uint8_t    tintChannel = 0;            // BiomeChannel
            uint32_t   tintConstant = 0xFFFFFF;

            // MC HalfTransparentBlock.skipRendering: a face touching a
            // neighbour of the SAME block is dropped. Non-opaque blocks are
            // invisible to the ordinary occlusion test, so without this every
            // internal boundary inside a glass wall or an ice sheet is drawn
            // and blended — which reads as murky, visibly layered glass.
            //
            // Identity, not "both translucent": vanilla checks
            // `neighborState.is(this)`, so glass against stained glass keeps
            // both faces.
            bool cullsAgainstSelf = false;

            // Index into s_ctmUVs, or -1 for the great majority of blocks that
            // have no connected-texture variants. Kept as a slot rather than
            // 16 inline UV rects because CachedBlockProps is a per-thread array
            // over every BlockID, and 256 bytes each would cost ~300 KB a
            // thread to serve eighteen glass types.
            int16_t ctmSlot = -1;
        };
        static constexpr size_t BLOCK_ID_COUNT = static_cast<size_t>(Game::BlockID::Count);
        static thread_local std::array<CachedBlockProps, BLOCK_ID_COUNT> s_blockPropsCache;
        static thread_local bool s_blockPropsCacheValid;
        // Atlas rects for a block's connected-texture tiles, indexed by
        // Render::CTM::SlotFor(). One entry per participating block (see
        // CachedBlockProps::ctmSlot). Sized to CTM::kMaxVariants (64) so the
        // header need not pull in ConnectedTextures.hpp; 47 slots are used.
        static thread_local std::vector<std::array<glm::vec4, 64>> s_ctmUVs;

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
        // Interior-only (no halo): a block's own state affects only its own
        // model. Neighbour lookups are for occlusion and AO, which depend on
        // the neighbour's block id, not its state.
        uint8_t m_stateCache[16][16][16];
        bool    m_stateCacheAllDefault = true;
        // Biome grid for this section (with the blend margin), or null when
        // meshing straight off an IBlockAccess. See ResolveBiome.
        const Client::Render::RegionSnapshot* m_biomeSource = nullptr;
        // Only set on the direct-access path, where biomes come from the world.
        const Game::IBlockAccess* m_biomeAccess = nullptr;

        int m_sectionBaseWorldX;
        int m_sectionBaseWorldY;
        int m_sectionBaseWorldZ;
        void FillBlockCacheFromAccess(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos, int sectionY);
        void FillBlockCacheFromRegion(const Client::Render::RegionSnapshot& region,
                                        Game::Math::ChunkPos chunkPos, int sectionY);
        void DeriveOpaqueCache();
        uint8_t CachedState(int localX, int sectionLocalY, int localZ) const {
            return m_stateCacheAllDefault ? 0 : m_stateCache[sectionLocalY][localZ][localX];
        }
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
                         int sectionY, Game::BlockID blockId, uint8_t stateIndex,
                         SectionMesh& mesh);

        // `stateIndex` is carried through because one tint resolver needs it:
        // MC colours melon/pumpkin stems by their AGE, so two stems of the same
        // BlockID are different colours. Reading it back out of `blocks` would
        // work but costs a world lookup per face for a value ProcessBlock
        // already has in hand.
        void AddBlockFace(const Game::IBlockAccess& blocks,
                         const Game::BlockModel& model, const Game::Element& element,
                         Game::FaceDir faceDir, const Game::FaceDef& faceDef,
                         glm::vec3 blockPos, glm::vec3 faceNormal, Game::BlockID blockId,
                         uint8_t stateIndex,
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
        // MC BiomeColors' four ColorResolvers.
        enum class BiomeChannel : uint8_t { Grass, Foliage, DryFoliage, Water };

        uint16_t  ResolveBiome(int worldX, int worldY, int worldZ) const;
        glm::vec4 BlendedBiomeTint(BiomeChannel channel,
                                   int worldX, int worldY, int worldZ) const;

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
                                              const glm::vec4& faceUv, int uvRotation);
        glm::vec3 GetFaceNormal(BlockFace face);

        // Minecraft-style per-vertex ambient occlusion
        // Returns a shade value 0.0-1.0 for a vertex corner based on 3 neighbor blocks
        float CalculateVertexAO(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ,
                                BlockFace face, int vertexIndex);

        // Minecraft directional face shading multiplier
        static float GetDirectionalShade(BlockFace face);

        // Which of the four in-plane neighbours of this face are the same
        // block, as a Render::CTM bitmask in TEXTURE space (left/right/top/
        // bottom of the sprite, not world axes). Drives connected glass.
        uint8_t ConnectedTextureMask(Game::BlockID blockId, BlockFace face,
                                     int worldX, int worldY, int worldZ) const;

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