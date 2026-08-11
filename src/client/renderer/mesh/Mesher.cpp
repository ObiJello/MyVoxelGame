// File: src/client/renderer/mesh/Mesher.cpp
#include "Mesher.hpp"
#include "MeshJobData.hpp"
#include "../culling/VisGraph.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/biome/Biomes.hpp"
#include "../texture/ConnectedTextures.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include <chrono>
#include <algorithm>
#include <cstring>

namespace Render {

    // Thread-local block property cache and UV cache definitions
    thread_local std::array<Mesher::CachedBlockProps, Mesher::BLOCK_ID_COUNT> Mesher::s_blockPropsCache{};
    thread_local bool Mesher::s_blockPropsCacheValid = false;
    thread_local std::vector<std::array<glm::vec4, 64>> Mesher::s_ctmUVs{};
    static_assert(CTM::kMaxVariants == 64,
                  "s_ctmUVs is declared with a literal 64 in Mesher.hpp to keep "
                  "ConnectedTextures.hpp out of that header - keep them in step");
    thread_local std::unordered_map<const Game::FaceDef*, glm::vec4> Mesher::s_faceUVCache;

    // Face normal vectors for each block face
    static const glm::vec3 FACE_NORMALS[] = {
        { 0.0f,  1.0f,  0.0f}, // PositiveY (Top)
        { 0.0f, -1.0f,  0.0f}, // NegativeY (Bottom)
        { 0.0f,  0.0f,  1.0f}, // PositiveZ (Front)
        { 0.0f,  0.0f, -1.0f}, // NegativeZ (Back)
        { 1.0f,  0.0f,  0.0f}, // PositiveX (Right)
        {-1.0f,  0.0f,  0.0f}  // NegativeX (Left)
    };

    // Face offset vectors for neighbor checking
    static const glm::ivec3 FACE_OFFSETS[] = {
        { 0,  1,  0}, // PositiveY
        { 0, -1,  0}, // NegativeY
        { 0,  0,  1}, // PositiveZ
        { 0,  0, -1}, // NegativeZ
        { 1,  0,  0}, // PositiveX
        {-1,  0,  0}  // NegativeX
    };

    // Read-only IBlockAccess adapter over the mesher's 18^3 block cache.
    // Passed to downstream code that still takes the interface (FluidMeshBuilder,
    // ProcessBlock/AddBlockFace signatures) — every GetBlock is a bounds check +
    // array read, replacing SnapshotBlockAccess's per-call neighbor-plane logic.
    // Positions outside the cached halo return Air (the fluid builder and AO
    // only ever sample within +/-1 of the section, which the halo covers).
    namespace {
        class CacheBlockAccess final : public Game::IBlockAccess {
        public:
            CacheBlockAccess(const Game::BlockID (&cache)[18][18][18],
                             int baseX, int baseY, int baseZ)
                : m_cache(cache), m_baseX(baseX), m_baseY(baseY), m_baseZ(baseZ) {}

            Game::BlockID GetBlock(int worldX, int worldY, int worldZ) const override {
                const int lx = worldX - m_baseX;
                const int ly = worldY - m_baseY;
                const int lz = worldZ - m_baseZ;
                if (lx < -1 || lx > 16 || ly < -1 || ly > 16 || lz < -1 || lz > 16) {
                    return Game::BlockID::Air;
                }
                return m_cache[ly + 1][lz + 1][lx + 1];
            }

            bool IsChunkLoaded(int, int) const override { return true; }

            bool IsPositionLoaded(int worldX, int worldY, int worldZ) const override {
                const int lx = worldX - m_baseX;
                const int ly = worldY - m_baseY;
                const int lz = worldZ - m_baseZ;
                return lx >= -1 && lx <= 16 && ly >= -1 && ly <= 16 && lz >= -1 && lz <= 16;
            }

            bool IsBlockSolid(int worldX, int worldY, int worldZ) const override {
                const Game::BlockID block = GetBlock(worldX, worldY, worldZ);
                return block != Game::BlockID::Air &&
                       block != Game::BlockID::Water &&
                       block != Game::BlockID::Lava;
            }

            bool IsBlockFluid(int worldX, int worldY, int worldZ) const override {
                const Game::BlockID block = GetBlock(worldX, worldY, worldZ);
                return block == Game::BlockID::Water || block == Game::BlockID::Lava;
            }

            bool IsValidPosition(int, int worldY, int) const override {
                return worldY >= Config::MinY && worldY <= Config::MaxY;
            }

        private:
            const Game::BlockID (&m_cache)[18][18][18];
            int m_baseX, m_baseY, m_baseZ;
        };
    }

    Mesher::Mesher(const MeshConfig& config) : m_config(config), m_world(nullptr) {
        m_lastStats = {};
        m_sectionBaseWorldX = 0;
        m_sectionBaseWorldY = 0;
        m_sectionBaseWorldZ = 0;
        std::memset(m_blockCache, 0, sizeof(m_blockCache));
        std::memset(m_opaqueCache, 0, sizeof(m_opaqueCache));
        m_fluidBuilder = std::make_unique<FluidMeshBuilder>();
        // Water is meshed by the fluid builder, which never reaches
        // AddBlockFace's tint dispatch — hand it the same blended water
        // resolver so it matches every other biome-tinted surface.
        m_fluidBuilder->waterTintProvider = [this](int x, int y, int z) {
            return m_config.enableBiomeTinting
                       ? BlendedBiomeTint(BiomeChannel::Water, x, y, z)
                       : glm::vec4(1.0f);
        };
    }

    void Mesher::SetWorld(Game::World* world) {
        m_world = world;
    }

    void Mesher::EnsureBlockPropsCache() {
        if (s_blockPropsCacheValid) return;

        // Rebuilt alongside the props it is indexed from; stale slots would
        // otherwise point at the previous atlas's rects after a reload.
        s_ctmUVs.clear();

        for (size_t i = 0; i < BLOCK_ID_COUNT; ++i) {
            auto blockId = static_cast<Game::BlockID>(i);
            const Game::Block& block = Game::BlockRegistry::Get(blockId);
            // For face culling we need "fully occludes its faces", not just
            // "made of opaque material." Partial-cube blocks (slabs, fences,
            // leaf litter, trapdoors, …) have their faces only partially
            // covering the neighbor's surface, so they MUST NOT cull the
            // neighbor's face — otherwise placing a slab next to a wall makes
            // the wall's whole side disappear. Mirrors MC's
            // BlockState.canOcclude() + the per-face shape check it does in
            // BlockBehaviour.skipRendering. v1 approximation: only full cubes
            // (shape == 0..1 on every axis) participate in face culling. Slab
            // tops/bottoms not culling the cube above/below them is a minor
            // hidden-face overdraw that we accept until a proper per-face
            // occlusion mask is added.
            // Default state is a valid representative here even for blocks
            // whose states rotate: a 90° turn about the cell centre maps the
            // unit cube onto itself, so "is this a full cube" can't change
            // between states. Occlusion stays a per-BlockID property.
            const auto& shape = Game::BlockRegistry::GetBlockShape(blockId);
            const bool fullCube =
                shape.min.x <= 0.0001f && shape.max.x >= 0.9999f &&
                shape.min.y <= 0.0001f && shape.max.y >= 0.9999f &&
                shape.min.z <= 0.0001f && shape.max.z >= 0.9999f;
            // BE-flagged blocks (chest, shulker, sign, banner, …) draw their
            // geometry via the BlockEntityRenderer, NOT via the chunk mesh.
            // From the chunk-mesh's POV the cell is empty even if `block.opaque`
            // is true and the model shape defaults to a full cube. Marking
            // them as non-occluding here prevents the neighbour-face cull
            // from punching visible holes in adjacent walls behind a chest.
            const bool beFlagged = Game::BlockEntityTypes::HasBlockEntity(blockId);
            s_blockPropsCache[i].isOpaque = block.opaque && fullCube && !beFlagged;
            s_blockPropsCache[i].hasStates = Game::BlockRegistry::HasStates(blockId);

            // MC's HalfTransparentBlock family — every block whose class chain
            // reaches HalfTransparentBlock overrides skipRendering to drop a
            // face shared with an identical neighbour (Blocks.java: glass and
            // the stained/tinted variants via TransparentBlock, ice /
            // frosted_ice / blue_ice, honey, slime, and the copper grates via
            // WaterloggedTransparentBlock).
            //
            // packed_ice is deliberately absent: vanilla registers it as a
            // plain Block, so it is opaque and already culls by occlusion.
            // Glass PANES are IronBarsBlock, whose skipRendering depends on the
            // per-side connection properties this engine does not model, so
            // they are left alone rather than guessed at.
            {
                const std::string& n = block.modelName;
                auto ends = [&](std::string_view suffix) {
                    return n.size() >= suffix.size() &&
                           n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0;
                };
                s_blockPropsCache[i].cullsAgainstSelf =
                    n == "glass" || n == "tinted_glass" || ends("_stained_glass") ||
                    n == "ice" || n == "frosted_ice" || n == "blue_ice" ||
                    n == "honey_block" || n == "slime_block" ||
                    ends("copper_grate");
            }

            // Connected textures. Resolve the 16 variant rects once per block
            // here rather than per face at mesh time; a miss (atlas built
            // without them) simply leaves ctmSlot at -1 and the block renders
            // with its plain sprite.
            s_blockPropsCache[i].ctmSlot = -1;
            if (CTM::IsConnected(block.modelName)) {
                const std::string baseKey = "block/" + block.modelName;
                std::array<glm::vec4, CTM::kMaxVariants> rects{};
                bool allFound = true;
                for (int slot = 0; slot < CTM::VariantCount(); ++slot) {
                    if (!GetTextureUV(CTM::VariantKey(baseKey, slot), rects[static_cast<size_t>(slot)])) {
                        allFound = false;
                        break;
                    }
                }
                if (allFound) {
                    s_ctmUVs.push_back(rects);
                    s_blockPropsCache[i].ctmSlot = static_cast<int16_t>(s_ctmUVs.size() - 1);
                }
            }

            // MC BlockColors.createDefault, in its own registration order.
            // Matched on model name so a snapshot bump picks new members of
            // each family up automatically, the way the mining/collision
            // classifiers in BlockRegistry already do.
            {
                using TS = CachedBlockProps::TintSource;
                const std::string& n = block.modelName;
                auto has = [&](std::string_view sub) { return n.find(sub) != std::string::npos; };
                auto is  = [&](std::string_view ex)  { return n == ex; };

                auto& p = s_blockPropsCache[i];
                p.tintSource = TS::None;

                // GRASS: grass_block, fern, short_grass, potted_fern, bush,
                // sugar_cane, and both halves of large_fern / tall_grass.
                //
                // The double plants are substring matches because this engine
                // splits each into two BlockIDs whose model names carry a
                // _bottom / _top suffix. MC samples the UPPER half's biome at
                // pos.below(); with biomes on a 4-block grid and a 5x5 blend,
                // one block of vertical offset changes the result only where a
                // biome border also happens to fall on a quart boundary, so the
                // plant's own position is used here.
                if (is("grass_block") || is("grass_block_snow") ||
                    is("fern") || is("short_grass") || is("potted_fern") ||
                    is("bush") || is("sugar_cane") ||
                    has("large_fern") || has("tall_grass")) {
                    p.tintSource = TS::Biome;
                    p.tintChannel = static_cast<uint8_t>(BiomeChannel::Grass);
                } else if (has("pink_petals") || has("wildflowers")) {
                    // tintIndex 0 is the petals (already coloured, untinted);
                    // anything else is the stem, which takes grass.
                    p.tintSource = TS::FlowerBed;
                } else if (is("spruce_leaves")) {
                    p.tintSource = TS::Constant;  // FoliageColor.FOLIAGE_EVERGREEN
                    p.tintConstant = 0x619961;
                } else if (is("birch_leaves")) {
                    p.tintSource = TS::Constant;  // FoliageColor.FOLIAGE_BIRCH
                    p.tintConstant = 0x80A755;
                } else if (has("leaf_litter")) {
                    p.tintSource = TS::Biome;
                    p.tintChannel = static_cast<uint8_t>(BiomeChannel::DryFoliage);
                } else if (is("oak_leaves") || is("jungle_leaves") ||
                           is("acacia_leaves") || is("dark_oak_leaves") ||
                           is("mangrove_leaves") || is("vine")) {
                    // EXACTLY MC's foliage list (BlockColors.java:47), not a
                    // "_leaves" substring. cherry_leaves and pale_oak_leaves
                    // carry tintindex 0 in their models but are NOT registered
                    // in vanilla, so they render from their own artwork —
                    // a substring match turns cherry blossom green.
                    p.tintSource = TS::Biome;
                    p.tintChannel = static_cast<uint8_t>(BiomeChannel::Foliage);
                } else if (is("water") || is("bubble_column") ||
                           has("water_cauldron")) {
                    p.tintSource = TS::Biome;
                    p.tintChannel = static_cast<uint8_t>(BiomeChannel::Water);
                } else if (is("lily_pad")) {
                    p.tintSource = TS::Constant;  // BlockColors.LILY_PAD_IN_WORLD
                    p.tintConstant = 0x208030;
                } else if (is("attached_melon_stem") || is("attached_pumpkin_stem")) {
                    p.tintSource = TS::Constant;  // BlockColors.java: -2046180
                    p.tintConstant = 0xE0C71C;
                }
            }
            switch (block.renderLayer) {
                case Game::RenderLayer::Cutout:      s_blockPropsCache[i].renderLayer = RenderLayer::Cutout; break;
                case Game::RenderLayer::Translucent:  s_blockPropsCache[i].renderLayer = RenderLayer::Translucent; break;
                default:                              s_blockPropsCache[i].renderLayer = RenderLayer::Opaque; break;
            }
        }
        s_blockPropsCacheValid = true;
    }

    void Mesher::FillBlockCacheFromAccess(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos, int sectionY) {
        PROFILE_ZONE;
        m_sectionBaseWorldX = chunkPos.x * 16;
        m_sectionBaseWorldY = sectionY * 16 + Config::MinY;
        m_sectionBaseWorldZ = chunkPos.z * 16;

        // Sample a 18x18x18 region: the section (16^3) plus a 1-block border on all sides.
        // This covers every neighbor position that AO and face culling will access.
        for (int ly = -1; ly <= 16; ++ly) {
            for (int lz = -1; lz <= 16; ++lz) {
                for (int lx = -1; lx <= 16; ++lx) {
                    int wx = m_sectionBaseWorldX + lx;
                    int wy = m_sectionBaseWorldY + ly;
                    int wz = m_sectionBaseWorldZ + lz;
                    m_blockCache[ly + 1][lz + 1][lx + 1] = blocks.GetBlock(wx, wy, wz);
                }
            }
        }

        m_biomeSource = nullptr;
        m_biomeAccess = &blocks;

        // Interior states only — the halo is used for occlusion/AO, which read
        // block ids, never states.
        m_stateCacheAllDefault = true;
        for (int ly = 0; ly < 16; ++ly) {
            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    const uint8_t st = blocks.GetBlockState(m_sectionBaseWorldX + lx,
                                                            m_sectionBaseWorldY + ly,
                                                            m_sectionBaseWorldZ + lz);
                    m_stateCache[ly][lz][lx] = st;
                    if (st != 0) m_stateCacheAllDefault = false;
                }
            }
        }
    }

    void Mesher::FillBlockCacheFromSnapshot(const Client::Render::SectionSnapshot& snapshot,
                                            Game::Math::ChunkPos chunkPos, int sectionY) {
        PROFILE_ZONE;
        m_sectionBaseWorldX = chunkPos.x * 16;
        m_sectionBaseWorldY = sectionY * 16 + Config::MinY;
        m_sectionBaseWorldZ = chunkPos.z * 16;

        // Snapshots leave `states` empty for sections that have none, which is
        // the overwhelming majority — keep the whole state path switched off in
        // that case rather than memcpying 4 KB of zeroes.
        m_biomeSource = &snapshot;
        m_biomeAccess = nullptr;

        m_stateCacheAllDefault = snapshot.states.empty();
        if (!m_stateCacheAllDefault) {
            for (int y = 0; y < 16; ++y)
                for (int z = 0; z < 16; ++z)
                    std::memcpy(&m_stateCache[y][z][0], &snapshot.states[y * 256 + z * 16], 16);
        }

        static_assert(sizeof(Game::BlockID) == sizeof(uint16_t),
                      "BlockID size changed — update FillBlockCacheFromSnapshot memcpys");
        constexpr size_t ROW_BYTES = 16 * sizeof(Game::BlockID);

        // Interior 16^3: snapshot layout is blocks[y*256 + z*16 + x] (x contiguous),
        // matching the cache's [y][z][x] layout — one memcpy per (y,z) row.
        for (int y = 0; y < 16; ++y) {
            for (int z = 0; z < 16; ++z) {
                std::memcpy(&m_blockCache[y + 1][z + 1][1],
                            &snapshot.blocks[y * 256 + z * 16], ROW_BYTES);
            }
        }

        // Axis-aligned halo faces from the neighbor boundary planes.
        // Plane indexing (see SectionSnapshot): N/S = [y*16+x], E/W = [y*16+z], U/D = [z*16+x].
        for (int z = 0; z < 16; ++z) {  // Down (ly=-1): neighbor below's y=15 plane
            std::memcpy(&m_blockCache[0][z + 1][1], &snapshot.neighbors[5][z * 16], ROW_BYTES);
        }
        for (int z = 0; z < 16; ++z) {  // Up (ly=16): neighbor above's y=0 plane
            std::memcpy(&m_blockCache[17][z + 1][1], &snapshot.neighbors[4][z * 16], ROW_BYTES);
        }
        for (int y = 0; y < 16; ++y) {  // North (lz=-1): north neighbor's z=15 plane
            std::memcpy(&m_blockCache[y + 1][0][1], &snapshot.neighbors[0][y * 16], ROW_BYTES);
        }
        for (int y = 0; y < 16; ++y) {  // South (lz=16): south neighbor's z=0 plane
            std::memcpy(&m_blockCache[y + 1][17][1], &snapshot.neighbors[1][y * 16], ROW_BYTES);
        }
        for (int y = 0; y < 16; ++y) {  // West (lx=-1) / East (lx=16): strided in z
            for (int z = 0; z < 16; ++z) {
                m_blockCache[y + 1][z + 1][0]  = snapshot.neighbors[3][y * 16 + z];
                m_blockCache[y + 1][z + 1][17] = snapshot.neighbors[2][y * 16 + z];
            }
        }

        // Halo edges/corners (2+ axes out of range): the snapshot only carries
        // face planes, so replicate SnapshotBlockAccess's dominant-axis rule
        // (priority Y > X > Z, other coordinates clamped into [0,15]). Each
        // such cell equals an already-filled face cell with the non-dominant
        // coordinates clamped — avoids bright AO seams at section borders.
        auto clampIn = [](int v) { return v < 0 ? 1 : (v > 15 ? 16 : v + 1); };
        for (int ly = -1; ly <= 16; ++ly) {
            const bool oy = (ly < 0 || ly > 15);
            for (int lz = -1; lz <= 16; ++lz) {
                const bool oz = (lz < 0 || lz > 15);
                for (int lx = -1; lx <= 16; ++lx) {
                    const bool ox = (lx < 0 || lx > 15);
                    if (static_cast<int>(oy) + static_cast<int>(oz) + static_cast<int>(ox) < 2)
                        continue;
                    Game::BlockID v;
                    if (oy)      v = m_blockCache[ly + 1][clampIn(lz)][clampIn(lx)];
                    else if (ox) v = m_blockCache[ly + 1][clampIn(lz)][lx + 1];
                    else         v = m_blockCache[ly + 1][lz + 1][clampIn(lx)];
                    m_blockCache[ly + 1][lz + 1][lx + 1] = v;
                }
            }
        }
    }

    void Mesher::DeriveOpaqueCache() {
        PROFILE_ZONE;
        // One pass over the 18^3 block cache: opacity via the per-thread props table
        const Game::BlockID* src = &m_blockCache[0][0][0];
        bool* dst = &m_opaqueCache[0][0][0];
        for (size_t i = 0; i < 18 * 18 * 18; ++i) {
            dst[i] = s_blockPropsCache[static_cast<uint16_t>(src[i])].isOpaque;
        }
    }

    bool Mesher::GetCachedOpaque(int worldX, int worldY, int worldZ) const {
        int lx = worldX - m_sectionBaseWorldX + 1;
        int ly = worldY - m_sectionBaseWorldY + 1;
        int lz = worldZ - m_sectionBaseWorldZ + 1;

        // Bounds check — positions outside the cached region fall back to non-opaque
        if (lx < 0 || lx >= 18 || ly < 0 || ly >= 18 || lz < 0 || lz >= 18) {
            return false;
        }
        return m_opaqueCache[ly][lz][lx];
    }

    Game::BlockID Mesher::GetCachedBlock(int worldX, int worldY, int worldZ) const {
        int lx = worldX - m_sectionBaseWorldX + 1;
        int ly = worldY - m_sectionBaseWorldY + 1;
        int lz = worldZ - m_sectionBaseWorldZ + 1;

        if (lx < 0 || lx >= 18 || ly < 0 || ly >= 18 || lz < 0 || lz >= 18) {
            return Game::BlockID::Air;
        }
        return m_blockCache[ly][lz][lx];
    }

    void Mesher::BuildSectionMesh(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos, int sectionY, SectionMesh& outMesh) {
        EnsureBlockPropsCache();
        FillBlockCacheFromAccess(blocks, chunkPos, sectionY);
        BuildSectionMeshFromCache(chunkPos, sectionY, outMesh);
    }

    void Mesher::BuildSectionMesh(const Client::Render::SectionSnapshot& snapshot,
                                  Game::Math::ChunkPos chunkPos, int sectionY, SectionMesh& outMesh) {
        EnsureBlockPropsCache();
        FillBlockCacheFromSnapshot(snapshot, chunkPos, sectionY);
        BuildSectionMeshFromCache(chunkPos, sectionY, outMesh);
    }

    void Mesher::BuildSectionMeshFromCache(Game::Math::ChunkPos chunkPos, int sectionY, SectionMesh& outMesh) {
        PROFILE_ZONE;
        auto startTime = std::chrono::high_resolution_clock::now();

        m_lastStats = {};

        outMesh.Clear();
        outMesh.chunkPos = chunkPos;
        outMesh.sectionY = sectionY;
        outMesh.Reserve(1024);

        // Derive the opacity cache from the freshly filled block cache — all
        // face culling and AO reads index this instead of registry lookups.
        DeriveOpaqueCache();

        // Adapter handed to downstream IBlockAccess consumers (fluid builder);
        // every read resolves to a cache array access.
        CacheBlockAccess cacheAccess(m_blockCache,
                                     m_sectionBaseWorldX, m_sectionBaseWorldY, m_sectionBaseWorldZ);

        // Build visibility graph from the opaque cache (indices offset by +1 for the border)
        VisGraph visGraph;
        for (int y = 0; y < 16; y++)
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++)
                    if (m_opaqueCache[y + 1][z + 1][x + 1])
                        visGraph.setOpaque(x, y, z);

        for (int localX = 0; localX < 16; ++localX) {
            for (int sectionLocalY = 0; sectionLocalY < 16; ++sectionLocalY) {
                for (int localZ = 0; localZ < 16; ++localZ) {
                    Game::BlockID blockId =
                        m_blockCache[sectionLocalY + 1][localZ + 1][localX + 1];

                    if (blockId != Game::BlockID::Air) {
                        int worldY = sectionY * 16 + sectionLocalY + Config::MinY;
                        ProcessBlock(cacheAccess, chunkPos, localX, worldY, localZ, sectionY, blockId,
                                     CachedState(localX, sectionLocalY, localZ), outMesh);
                    }
                }
            }
        }

        // Resolve visibility graph (which face pairs can see through this section)
        outMesh.visibilitySet = visGraph.resolve();

        auto endTime = std::chrono::high_resolution_clock::now();
        m_lastStats.buildTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    }

    // BuildChunkMesh removed - use BuildSectionMesh with IBlockAccess instead

    // FaceDir (model JSON) and BlockFace (mesher) number their horizontal
    // entries in OPPOSITE orders — FaceDir is North(-Z)=2, South(+Z)=3,
    // West(-X)=4, East(+X)=5, while BlockFace is PositiveZ=2, NegativeZ=3,
    // PositiveX=4, NegativeX=5. Casting one to the other therefore swapped
    // north<->south and east<->west, i.e. rotated every model's side textures
    // by 180 degrees about Y.
    //
    // This was invisible for years because virtually every block is cube_all
    // with four identical side textures. It only shows up on models whose
    // sides differ — a furnace's front ended up on the block's back face, so a
    // correctly-oriented furnace still looked like it faced away from you.
    static constexpr BlockFace FaceDirToBlockFace(Game::FaceDir dir) {
        switch (dir) {
            case Game::FaceDir::Up:    return BlockFace::PositiveY;
            case Game::FaceDir::Down:  return BlockFace::NegativeY;
            case Game::FaceDir::North: return BlockFace::NegativeZ;
            case Game::FaceDir::South: return BlockFace::PositiveZ;
            case Game::FaceDir::West:  return BlockFace::NegativeX;
            case Game::FaceDir::East:  return BlockFace::PositiveX;
        }
        return BlockFace::PositiveY;
    }

    void Mesher::ProcessBlock(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos,
                             int localX, int worldY, int localZ,
                             int sectionY, Game::BlockID blockId, uint8_t stateIndex,
                             SectionMesh& mesh) {

        int worldX = chunkPos.x * 16 + localX;
        int worldZ = chunkPos.z * 16 + localZ;
        // blockId passed from caller — avoids redundant GetBlock() virtual call

        // Handle fluid blocks separately
        if (blockId == Game::BlockID::Water || blockId == Game::BlockID::Lava) {
            if (m_fluidBuilder) {
                m_fluidBuilder->BuildFluidBlock(blocks, chunkPos, worldX, worldY, worldZ, mesh);
                m_lastStats.facesGenerated++;
            }
            return;
        }

        // Model is keyed on (block, state) — the blockstate JSON maps each
        // state to its own, possibly pre-rotated, model. MC's equivalent lookup
        // is BlockModelShaper.getBlockModel(BlockState).
        const Game::BlockModel& model = Game::BlockRegistry::GetBlockModel(blockId, stateIndex);
        glm::vec3 worldPos = LocalToWorldPos(chunkPos, localX, worldY, localZ);

        // Use cached render layer instead of registry lookup
        RenderLayer blockLayer = s_blockPropsCache[static_cast<uint16_t>(blockId)].renderLayer;

        for (const auto& element : model.elements) {
            for (const auto& [faceDir, faceDef] : element.faces) {
                BlockFace blockFace = FaceDirToBlockFace(faceDir);

                // Face culling uses the pre-built opaque cache (no GetBlock/registry calls).
                //
                // Only faces that DECLARE a cullface participate, and the
                // neighbour tested is the one that cullface names — not the
                // face's own normal. MC keeps a @Nullable cull direction per
                // quad (BlockElementFace.cullForDirection) and skips the whole
                // check when it is absent, so a face authored without one is
                // always drawn.
                //
                // Culling on the face direction instead dropped geometry MC
                // keeps: any inner face of a multi-element model that happens to
                // point at a solid neighbour, and every rotated model whose
                // cullface was turned to a different direction than its normal
                // (RotateModel rewrites the cullface for exactly that reason,
                // and that rewrite was being thrown away here).
                if (m_config.enableFaceCulling && faceDef.cullfaceDir >= 0) {
                    const BlockFace cullAgainst =
                        FaceDirToBlockFace(static_cast<Game::FaceDir>(faceDef.cullfaceDir));
                    if (ShouldCullFace(worldX, worldY, worldZ, cullAgainst)) {
                        m_lastStats.facesCulled++;
                        continue;
                    }
                    // MC Block.shouldRenderFace consults skipRendering as well
                    // as occlusion, and both gate on the same cullface quad.
                    // HalfTransparentBlock answers true for an identical
                    // neighbour, which is what keeps a glass wall or an ice
                    // sheet a single clean surface instead of a stack of
                    // blended internal panes.
                    if (s_blockPropsCache[static_cast<size_t>(blockId)].cullsAgainstSelf) {
                        const glm::ivec3& off = FACE_OFFSETS[static_cast<int>(cullAgainst)];
                        if (GetCachedBlock(worldX + off.x, worldY + off.y, worldZ + off.z) == blockId) {
                            m_lastStats.facesCulled++;
                            continue;
                        }
                    }
                }

                glm::vec3 faceNormal = GetFaceNormal(blockFace);
                AddBlockFace(blocks, model, element, faceDir, faceDef, worldPos, faceNormal, blockId, worldX, worldY, worldZ, blockLayer, mesh);
                m_lastStats.facesGenerated++;
            }
        }
    }

    void Mesher::AddBlockFace(const Game::IBlockAccess& blocks,
                             const Game::BlockModel& model, const Game::Element& element,
                             Game::FaceDir faceDir, const Game::FaceDef& faceDef,
                             glm::vec3 blockPos, glm::vec3 faceNormal, Game::BlockID blockId,
                             int worldX, int worldY, int worldZ, RenderLayer layer, SectionMesh& mesh) {

        // Lookup UV rect from thread-local cache (persists across Mesher instances,
        // so texture resolution only happens once per FaceDef per thread lifetime)
        glm::vec4 uvRect;
        const int16_t ctmSlot = s_blockPropsCache[static_cast<size_t>(blockId)].ctmSlot;
        if (ctmSlot >= 0) {
            // Connected textures pick a different tile per FACE POSITION, so
            // they cannot use the per-FaceDef cache below — that cache is keyed
            // on the model's face and is shared by every block in the world.
            const uint8_t mask = ConnectedTextureMask(blockId, FaceDirToBlockFace(faceDir),
                                                      worldX, worldY, worldZ);
            uvRect = s_ctmUVs[static_cast<size_t>(ctmSlot)]
                             [static_cast<size_t>(CTM::SlotFor(mask))];
        } else if (auto cacheIt = s_faceUVCache.find(&faceDef); cacheIt != s_faceUVCache.end()) {
            uvRect = cacheIt->second;
        } else {
            std::string texturePath = model.ResolveTexture(faceDef.textureRef);
            if (!GetTextureUV(texturePath, uvRect)) {
                GetTextureUV("missingno", uvRect);
            }
            s_faceUVCache[&faceDef] = uvRect;
        }

        // **FIXED**: Calculate biome tint based on tintIndex
        glm::vec4 tintColor(1.0f, 1.0f, 1.0f, 1.0f); // Default white (no tint)

        // MC BlockColors.getColor(state, level, pos, tintIndex): the BLOCK
        // selects the resolver, and tintIndex only filters inside it. A face
        // whose block has no registered resolver stays white — vanilla's -1.
        if (m_config.enableBiomeTinting && faceDef.tintIndex >= 0) {
            using TS = CachedBlockProps::TintSource;
            const auto& props = s_blockPropsCache[static_cast<size_t>(blockId)];
            switch (props.tintSource) {
                case TS::None:
                    break;
                case TS::Constant:
                    tintColor = glm::vec4(((props.tintConstant >> 16) & 0xFF) / 255.0f,
                                          ((props.tintConstant >> 8) & 0xFF) / 255.0f,
                                          (props.tintConstant & 0xFF) / 255.0f, 1.0f);
                    break;
                case TS::Biome:
                    tintColor = BlendedBiomeTint(static_cast<BiomeChannel>(props.tintChannel),
                                                 worldX, worldY, worldZ);
                    break;
                case TS::FlowerBed:
                    // BlockColors.java:37-42 — tintIndex 0 returns -1.
                    if (faceDef.tintIndex != 0) {
                        tintColor = BlendedBiomeTint(BiomeChannel::Grass, worldX, worldY, worldZ);
                    }
                    break;
            }
        }

        // Convert face direction to our BlockFace enum
        BlockFace blockFace = FaceDirToBlockFace(faceDir);

        // Convert element from MC pixel-space [0,16] → block-space [0,1] so we can
        // build vertices in world units. Without this, partial-cube models (leaf
        // litter, carpets, slabs, fences, …) silently rendered as full 1×1×1 cubes.
        const glm::vec3 elemMin = element.from * (1.0f / 16.0f);
        const glm::vec3 elemMax = element.to   * (1.0f / 16.0f);

        // Create face vertices (stack-allocated, no heap alloc)
        std::array<Vertex, 4> faceVerts = CreateFaceVertices(blockPos, blockFace, uvRect, tintColor,
                                                             elemMin, elemMax, faceDef.uv,
                                                             faceDef.uvRotation);

        // MC FaceBakery.applyElementRotation, applied per vertex because the
        // angle is arbitrary and cannot be folded back into an axis-aligned
        // from/to. Never applied before, which is why the flowerbed stems sat
        // nowhere near their flowers: pink petals and wildflowers author each
        // stem out beyond the block (flowerbed_3 puts one at x≈17.65) and rely
        // on a -45° turn about the corner to swing it in beside the petals.
        if (!element.rotation.IsIdentity()) {
            for (Vertex& v : faceVerts) {
                v.pos = blockPos + Game::ApplyElementRotation(v.pos - blockPos,
                                                              element.rotation,
                                                              1.0f / 16.0f);
            }
        }

        // MC BlockRenderDispatcher.renderBatched: the block's offset is added
        // to the pose before the quads are emitted. Purely visual — collision
        // and the selection box stay on the grid, exactly as in vanilla.
        //
        // Without it every flower and tuft of grass sits dead centre in its
        // cell, and a meadow reads as a lattice instead of scatter.
        if (const glm::vec3 offset =
                Game::BlockRegistry::GetBlockOffset(blockId, worldX, worldZ);
            offset != glm::vec3(0.0f)) {
            for (Vertex& v : faceVerts) v.pos += offset;
        }

        // Bake AO and directional shading into vertex colors (Minecraft-style)
        // All math is in gamma space — shade values are direct multipliers, matching Minecraft.
        //
        // Both multipliers are OPT-OUT in vanilla, on two different scopes:
        //
        //   • `shade` is per ELEMENT. ModelBlockRenderer.java:259 passes it to
        //     ClientLevel.getShade(dir, shade), which returns a flat 1.0 for
        //     every direction when it is false (ClientLevel.java:722) instead of
        //     the 0.8 / 0.6 / 0.5 face table.
        //   • `ambientocclusion` is per MODEL (ModelBlockRenderer.java:42).
        //
        // Vanilla's cross-shaped plant parents — block/cross, block/tinted_cross,
        // block/crop and friends — turn BOTH off. Applying them anyway is what
        // made short_grass/tall_grass render at 0.36-0.8 brightness with the two
        // crossed planes at visibly different shades and their bases darkened
        // against the ground block, where vanilla draws them uniformly at 1.0.
        const float directionalShade = element.shade ? GetDirectionalShade(blockFace) : 1.0f;
        const bool  useAO = model.ambientOcclusion;
        for (int v = 0; v < 4; ++v) {
            float aoShade = useAO
                                ? CalculateVertexAO(blocks, worldX, worldY, worldZ, blockFace, v)
                                : 1.0f;
            float finalShade = aoShade * directionalShade;
            glm::vec4 c = faceVerts[v].GetColor();
            c.r *= finalShade;
            c.g *= finalShade;
            c.b *= finalShade;
            faceVerts[v].SetColor(c);
        }

        // Add to appropriate mesh layer
        switch (layer) {
            case RenderLayer::Opaque:
                GenerateQuad(faceVerts, mesh.opaqueVerts, mesh.opaqueIdxs);
                break;
            case RenderLayer::Cutout:
                GenerateQuad(faceVerts, mesh.cutoutVerts, mesh.cutoutIdxs);
                break;
            case RenderLayer::Translucent:
                GenerateQuad(faceVerts, mesh.translucentVerts, mesh.translucentIdxs);
                break;
        }

        m_lastStats.quadsGenerated++;
    }

    uint16_t Mesher::ResolveBiome(int worldX, int worldY, int worldZ) const {
        if (m_biomeSource) {
            return m_biomeSource->GetBiomeLocal(worldX - m_sectionBaseWorldX,
                                                worldY - m_sectionBaseWorldY,
                                                worldZ - m_sectionBaseWorldZ);
        }
        return m_biomeAccess ? m_biomeAccess->GetBiome(worldX, worldY, worldZ) : 0;
    }

    // MC ClientLevel.calculateBlockTint — the biome blend.
    //
    //   int radius = options.biomeBlendRadius().get();          // default 2
    //   if (radius == 0) return resolver.getColor(biome(pos), x, z);
    //   int size = (radius*2+1)^2;
    //   ...accumulate r/g/b over the square at THIS y...
    //   return (r/size)<<16 | (g/size)<<8 | (b/size);
    //
    // The square is horizontal only — every sample uses the block's own Y. The
    // per-channel integer average (not a colour-space blend) is what gives
    // vanilla its characteristic 5-block-wide biome gradient, so anything
    // cheaper here shows up as a hard seam at every biome border.
    glm::vec4 Mesher::BlendedBiomeTint(BiomeChannel channel,
                                       int worldX, int worldY, int worldZ) const {
        constexpr int kRadius = 2;    // Options.biomeBlendRadius default
        constexpr int kSize = (kRadius * 2 + 1) * (kRadius * 2 + 1);

        int r = 0, g = 0, b = 0;
        for (int dz = -kRadius; dz <= kRadius; ++dz) {
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                const int sx = worldX + dx;
                const int sz = worldZ + dz;
                const uint16_t biome = ResolveBiome(sx, worldY, sz);

                uint32_t c = 0;
                switch (channel) {
                    case BiomeChannel::Grass:
                        c = Game::BiomeRegistry::GrassColor(biome, sx, sz);
                        break;
                    case BiomeChannel::Foliage:
                        c = Game::BiomeRegistry::FoliageColor(biome);
                        break;
                    case BiomeChannel::DryFoliage:
                        c = Game::BiomeRegistry::DryFoliageColor(biome);
                        break;
                    case BiomeChannel::Water:
                        c = Game::BiomeRegistry::WaterColor(biome);
                        break;
                }
                r += (c >> 16) & 0xFF;
                g += (c >> 8) & 0xFF;
                b += c & 0xFF;
            }
        }
        return glm::vec4((r / kSize) / 255.0f,
                         (g / kSize) / 255.0f,
                         (b / kSize) / 255.0f, 1.0f);
    }

    // **NEW**: Grass-specific tinting (tint index 1)
    glm::vec4 Mesher::CalculateGrassTint(Game::BlockID blockId, int worldX, int worldY, int worldZ) {
        // tintindex 1 appears on exactly four models in the whole asset set —
        // flowerbed_1..4, the shared parent of pink petals and wildflowers —
        // and only ever on their STEM faces. MC's resolver for those blocks is
        // (BlockColors.java:37-42):
        //
        //     tintIndex != 0 ? BiomeColors.getAverageGrassColor(level, pos)
        //                    : -1
        //
        // so the stems take the grass colour while the petals stay untinted
        // (their texture is already coloured; the stem texture is greyscale,
        // avg 169,169,169, and is purely a tint carrier).
        //
        // No biome data reaches the mesher, so this is MC's own biome-less
        // answer: GrassColor.getDefaultColor() = get(0.5, 1.0), i.e.
        // textures/colormap/grass.png[(127, 127)] = #7CBD6B. The previous
        // #80B34D was a hand-picked green, noticeably darker and duller than
        // anything the colormap actually produces.
        (void)blockId;
        return glm::vec4(124.0f / 255.0f, 189.0f / 255.0f, 107.0f / 255.0f, 1.0f);
    }

    // **NEW**: Foliage-specific tinting (tint index 0)
    glm::vec4 Mesher::CalculateFoliageTint(Game::BlockID blockId, int worldX, int worldY, int worldZ) {
        switch (blockId) {
            // Leaf litter is the one block MC resolves through the DRY foliage
            // colormap rather than the foliage one — BlockColors.java:48 gives
            // it BiomeColors.getAverageDryFoliageColor while every other
            // tintindex-0 block on line 47 gets getAverageFoliageColor. Falling
            // through to the green default below is why it came out looking
            // like leaves scattered on the ground instead of dead litter.
            //
            // MC samples textures/colormap/dry_foliage.png at the biome's
            // (temperature, downfall) via ColorMapColorUtil.get. Nothing in this
            // mesher has biome data — every tint in this function is a constant —
            // so this is that colormap sampled at temperate-forest parameters
            // (0.7, 0.8), which is where leaf litter actually generates:
            // dry_foliage.png[(51, 217)] = #A36D46.
            //
            // Deliberately NOT DryFoliageColor.FOLIAGE_DRY_DEFAULT (#5C3C32):
            // that is MC's no-level fallback for inventory icons, far darker
            // than anything the colormap yields in world.
            case Game::BlockID::LeafLitter:
            case Game::BlockID::LeafLitter2:
            case Game::BlockID::LeafLitter3:
            case Game::BlockID::LeafLitter4:
                return glm::vec4(163.0f / 255.0f, 109.0f / 255.0f, 70.0f / 255.0f, 1.0f);

            case Game::BlockID::OakLeaves:
                return glm::vec4(0.4f, 0.7f, 0.2f, 1.0f); // Oak leaves green
            case Game::BlockID::BirchLeaves:
                return glm::vec4(0.6f, 0.9f, 0.5f, 1.0f); // Brighter cherry green
            default:
                return glm::vec4(0.4f, 0.6f, 0.2f, 1.0f); // Default foliage green
        }
    }

    // **UPDATED**: General biome tinting
    glm::vec4 Mesher::CalculateBiomeTint(Game::BlockID blockId, int worldX, int worldY, int worldZ) {
        // This is where you'd implement full biome-based tinting
        // For now, return a neutral tint
        return glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    void Mesher::GenerateQuad(const std::array<Vertex, 4>& quadVerts,
                             std::vector<Vertex>& outVerts, std::vector<uint16_t>& outIndices) {
        // 16-bit index guard: a section layer cannot exceed 65,536 vertices.
        // Unreachable for real content (worst-case checkerboard is ~49k verts);
        // dropping the quad beats corrupting indices if it ever happens.
        if (outVerts.size() + 4 > 65536) {
            return;
        }
        uint16_t baseIndex = static_cast<uint16_t>(outVerts.size());

        // Add vertices
        outVerts.insert(outVerts.end(), quadVerts.begin(), quadVerts.end());

        // **FIXED**: Correct triangle winding for counter-clockwise faces (viewed from outside)
        // Vertices are ordered: 0=bottom-left, 1=bottom-right, 2=top-right, 3=top-left
        outIndices.insert(outIndices.end(), {
            static_cast<uint16_t>(baseIndex + 0), static_cast<uint16_t>(baseIndex + 1),
            static_cast<uint16_t>(baseIndex + 2),  // First triangle: 0->1->2
            static_cast<uint16_t>(baseIndex + 0), static_cast<uint16_t>(baseIndex + 2),
            static_cast<uint16_t>(baseIndex + 3)   // Second triangle: 0->2->3
        });
    }

    bool Mesher::ShouldCullFace(int worldX, int worldY, int worldZ, BlockFace face) {
        // Use the pre-built opaque cache: neighbor is opaque → cull this face.
        // The offset table gives us the neighbor position directly.
        const glm::ivec3& offset = FACE_OFFSETS[static_cast<int>(face)];
        return GetCachedOpaque(worldX + offset.x, worldY + offset.y, worldZ + offset.z);
    }

    // **UPDATED**: Now supports cross-chunk neighbor lookup via IBlockAccess
    Game::BlockID Mesher::GetNeighborBlock(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ,
                                          BlockFace face) {
        glm::ivec3 offset = FACE_OFFSETS[static_cast<int>(face)];

        // Calculate neighbor position in world coordinates
        int neighborX = worldX + offset.x;
        int neighborY = worldY + offset.y;
        int neighborZ = worldZ + offset.z;

        // IBlockAccess handles cross-chunk boundaries automatically
        return blocks.GetBlock(neighborX, neighborY, neighborZ);
    }

    bool Mesher::GetTextureUV(const std::string& texturePath, glm::vec4& uvRect) {
        if (g_atlasBuilder) {
            AtlasUVRect atlasUV;
            if (g_atlasBuilder->GetUVRect(texturePath, atlasUV)) {
                uvRect = glm::vec4(atlasUV.uvMin.x, atlasUV.uvMin.y,
                                  atlasUV.uvMax.x, atlasUV.uvMax.y);
                return true;
            }
        }

        // Fallback to error texture
        uvRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        return false;
    }

    std::array<Vertex, 4> Mesher::CreateFaceVertices(glm::vec3 blockPos, BlockFace face,
                                                  const glm::vec4& uvRect, const glm::vec4& tint,
                                                  const glm::vec3& elemMin, const glm::vec3& elemMax,
                                                  const glm::vec4& faceUv, int uvRotation) {
        std::array<Vertex, 4> vertices;
        glm::vec3 normal = GetFaceNormal(face);

        // Map MC's per-face `uv` field (pixels 0..16) into atlas-space UVs by
        // interpolating within the texture's atlas sub-rect `uvRect`. The
        // existing per-face winding code below maps:
        //   uvRect.x → "u1" (left of atlas),  uvRect.z → "u2" (right)
        //   uvRect.y → "v1" (top of atlas),   uvRect.w → "v2" (bottom)
        // so we just lerp the faceUv pixel coords into those bounds and swap
        // the symbols into the same vMin/vMax slots the old code used.
        const float uSpan = uvRect.z - uvRect.x;
        const float vSpan = uvRect.w - uvRect.y;
        const float u1 = uvRect.x + (faceUv.x / 16.0f) * uSpan;
        const float v1 = uvRect.y + (faceUv.y / 16.0f) * vSpan;
        const float u2 = uvRect.x + (faceUv.z / 16.0f) * uSpan;
        const float v2 = uvRect.y + (faceUv.w / 16.0f) * vSpan;

        // MC's per-face `"rotation"` field (BlockElementFace.getU/getV) is a
        // pure permutation of which UV corner each of the four vertices gets:
        //   getU(uvs, rot, vertex) = uvs.getVertexU(rot.rotateVertexIndex(vertex))
        //   rotateVertexIndex(i)   = (i + shift) % 4        [Quadrant.java:72]
        // No trig, no matrix. Every face case below emits its corners in the
        // same order — (u1,v2), (u2,v2), (u2,v1), (u1,v1) — so the permutation
        // applies uniformly here rather than six times over.
        //
        // Our corner order is a cyclic relabelling of MC's (our vertex k is MC
        // index (k+1)%4), and a cyclic shift commutes with a cyclic relabelling,
        // so the shift is applied with the same (k + shift) % 4 MC uses.
        const glm::vec2 corners[4] = {
            glm::vec2(u1, v2), glm::vec2(u2, v2), glm::vec2(u2, v1), glm::vec2(u1, v1)
        };
        const int shift = ((uvRotation / 90) % 4 + 4) % 4;
        const glm::vec2 c0 = corners[(0 + shift) & 3];
        const glm::vec2 c1 = corners[(1 + shift) & 3];
        const glm::vec2 c2 = corners[(2 + shift) & 3];
        const glm::vec2 c3 = corners[(3 + shift) & 3];

        const float xMin = elemMin.x, yMin = elemMin.y, zMin = elemMin.z;
        const float xMax = elemMax.x, yMax = elemMax.y, zMax = elemMax.z;

        // Exact element-bounded positions. Full-cube blocks (elemMin=(0,0,0),
        // elemMax=(1,1,1), faceUv=(0,0,16,16)) reproduce the previous hardcoded
        // [0,1] cube behaviour exactly; partial models (leaf litter, carpets,
        // slabs, fences, …) now render at their true geometry instead of being
        // silently stretched to a full cube.
        switch (face) {
            case BlockFace::PositiveY: // Top face (+Y)
                // MC FaceBakery convention (FaceInfo.UP + BlockElementFace.getU/getV):
                //   minZ (north/back) → vMin (top of texture);
                //   maxZ (south/front) → vMax (bottom of texture).
                // i.e. "north is up" when looking down at the texture. Sides (NSEW) match
                // MC already; only top/bottom were V-flipped before. Without this, blocks
                // with directional top textures (beacon glass, stripped logs, sandstone)
                // render with their top rotated 180° from MC.
                vertices[0] = Vertex(blockPos + glm::vec3(xMin, yMax, zMax), normal, c0, tint);
                vertices[1] = Vertex(blockPos + glm::vec3(xMax, yMax, zMax), normal, c1, tint);
                vertices[2] = Vertex(blockPos + glm::vec3(xMax, yMax, zMin), normal, c2, tint);
                vertices[3] = Vertex(blockPos + glm::vec3(xMin, yMax, zMin), normal, c3, tint);
                break;

            case BlockFace::NegativeY: // Bottom face (-Y)
                // MC FaceBakery (FaceInfo.DOWN): maxZ → vMin, minZ → vMax. Inverse of UP.
                vertices[0] = Vertex(blockPos + glm::vec3(xMin, yMin, zMin), normal, c0, tint);
                vertices[1] = Vertex(blockPos + glm::vec3(xMax, yMin, zMin), normal, c1, tint);
                vertices[2] = Vertex(blockPos + glm::vec3(xMax, yMin, zMax), normal, c2, tint);
                vertices[3] = Vertex(blockPos + glm::vec3(xMin, yMin, zMax), normal, c3, tint);
                break;

            case BlockFace::PositiveZ: // Front face (+Z)
                vertices[0] = Vertex(blockPos + glm::vec3(xMin, yMin, zMax), normal, c0, tint);
                vertices[1] = Vertex(blockPos + glm::vec3(xMax, yMin, zMax), normal, c1, tint);
                vertices[2] = Vertex(blockPos + glm::vec3(xMax, yMax, zMax), normal, c2, tint);
                vertices[3] = Vertex(blockPos + glm::vec3(xMin, yMax, zMax), normal, c3, tint);
                break;

            case BlockFace::NegativeZ: // Back face (-Z)
                vertices[0] = Vertex(blockPos + glm::vec3(xMax, yMin, zMin), normal, c0, tint);
                vertices[1] = Vertex(blockPos + glm::vec3(xMin, yMin, zMin), normal, c1, tint);
                vertices[2] = Vertex(blockPos + glm::vec3(xMin, yMax, zMin), normal, c2, tint);
                vertices[3] = Vertex(blockPos + glm::vec3(xMax, yMax, zMin), normal, c3, tint);
                break;

            case BlockFace::PositiveX: // Right face (+X)
                vertices[0] = Vertex(blockPos + glm::vec3(xMax, yMin, zMax), normal, c0, tint);
                vertices[1] = Vertex(blockPos + glm::vec3(xMax, yMin, zMin), normal, c1, tint);
                vertices[2] = Vertex(blockPos + glm::vec3(xMax, yMax, zMin), normal, c2, tint);
                vertices[3] = Vertex(blockPos + glm::vec3(xMax, yMax, zMax), normal, c3, tint);
                break;

            case BlockFace::NegativeX: // Left face (-X)
                vertices[0] = Vertex(blockPos + glm::vec3(xMin, yMin, zMin), normal, c0, tint);
                vertices[1] = Vertex(blockPos + glm::vec3(xMin, yMin, zMax), normal, c1, tint);
                vertices[2] = Vertex(blockPos + glm::vec3(xMin, yMax, zMax), normal, c2, tint);
                vertices[3] = Vertex(blockPos + glm::vec3(xMin, yMax, zMin), normal, c3, tint);
                break;
        }

        return vertices;
    }

    glm::vec3 Mesher::GetFaceNormal(BlockFace face) {
        return FACE_NORMALS[static_cast<int>(face)];
    }

    // Minecraft directional face shading (from ClientLevel.java getShade())
    uint8_t Mesher::ConnectedTextureMask(Game::BlockID blockId, BlockFace face,
                                         int worldX, int worldY, int worldZ) const {
        // World direction of the sprite's +U (right) and +V-up axes for each
        // face. Read straight off CreateFaceVertices, which lays every face out
        // as v0=(u1,v2) bottom-left, v1=(u2,v2) bottom-right, v2=(u2,v1)
        // top-right — so v0→v1 is texture-right and v1→v2 is texture-up.
        // Getting these wrong erases the frame on the wrong edge, which shows
        // up as seams running the wrong way across a wall.
        struct Axes { glm::ivec3 right, up; };
        static const Axes kAxes[6] = {
            /* PositiveY */ { { 1, 0,  0}, { 0, 0, -1} },
            /* NegativeY */ { { 1, 0,  0}, { 0, 0,  1} },
            /* PositiveZ */ { { 1, 0,  0}, { 0, 1,  0} },
            /* NegativeZ */ { {-1, 0,  0}, { 0, 1,  0} },
            /* PositiveX */ { { 0, 0, -1}, { 0, 1,  0} },
            /* NegativeX */ { { 0, 0,  1}, { 0, 1,  0} },
        };
        const Axes& ax = kAxes[static_cast<int>(face)];

        auto same = [&](const glm::ivec3& d) {
            return GetCachedBlock(worldX + d.x, worldY + d.y, worldZ + d.z) == blockId;
        };

        uint8_t mask = 0;
        if (same(-ax.right)) mask |= CTM::LEFT;
        if (same( ax.right)) mask |= CTM::RIGHT;
        if (same( ax.up))    mask |= CTM::TOP;
        if (same(-ax.up))    mask |= CTM::BOTTOM;
        // Diagonals decide the corner pixels: at a concave corner both edges
        // connect but the diagonal cell is empty, and the corner is what closes
        // the outline around the notch.
        if (same(-ax.right + ax.up)) mask |= CTM::TL;
        if (same( ax.right + ax.up)) mask |= CTM::TR;
        if (same(-ax.right - ax.up)) mask |= CTM::BL;
        if (same( ax.right - ax.up)) mask |= CTM::BR;
        return mask;
    }

    float Mesher::GetDirectionalShade(BlockFace face) {
        switch (face) {
            case BlockFace::PositiveY: return 1.0f;  // UP
            case BlockFace::NegativeY: return 0.5f;  // DOWN
            case BlockFace::PositiveZ: return 0.8f;  // SOUTH
            case BlockFace::NegativeZ: return 0.8f;  // NORTH
            case BlockFace::PositiveX: return 0.6f;  // EAST
            case BlockFace::NegativeX: return 0.6f;  // WEST
            default: return 1.0f;
        }
    }

    // Per-vertex AO neighbor offset tables.
    // For each face direction, for each vertex (0-3), defines:
    //   {edge1_dx, edge1_dy, edge1_dz, edge2_dx, edge2_dy, edge2_dz, corner_dx, corner_dy, corner_dz}
    // The offsets are relative to the block position, shifted by the face normal.
    // Vertex ordering matches CreateFaceVertices() winding.

    struct AOVertexNeighbors {
        glm::ivec3 edge1;
        glm::ivec3 edge2;
        glm::ivec3 corner;
    };

    // For each face, 4 vertices, each with 2 edge neighbors and 1 corner neighbor
    // All offsets include the face normal direction (we sample in the plane one step out from the face)
    static const AOVertexNeighbors AO_NEIGHBORS[6][4] = {
        // PositiveY (Top face, +Y) — vertices: 0=front-left, 1=front-right, 2=back-right, 3=back-left
        {
            { {-1, 1, 0}, { 0, 1, 1}, {-1, 1, 1} },  // v0: west + south edges, SW corner
            { { 1, 1, 0}, { 0, 1, 1}, { 1, 1, 1} },  // v1: east + south edges, SE corner
            { { 1, 1, 0}, { 0, 1,-1}, { 1, 1,-1} },  // v2: east + north edges, NE corner
            { {-1, 1, 0}, { 0, 1,-1}, {-1, 1,-1} },  // v3: west + north edges, NW corner
        },
        // NegativeY (Bottom face, -Y) — vertices: 0=back-left, 1=back-right, 2=front-right, 3=front-left
        {
            { {-1,-1, 0}, { 0,-1,-1}, {-1,-1,-1} },  // v0
            { { 1,-1, 0}, { 0,-1,-1}, { 1,-1,-1} },  // v1
            { { 1,-1, 0}, { 0,-1, 1}, { 1,-1, 1} },  // v2
            { {-1,-1, 0}, { 0,-1, 1}, {-1,-1, 1} },  // v3
        },
        // PositiveZ (Front/South face, +Z) — vertices: 0=bottom-left, 1=bottom-right, 2=top-right, 3=top-left
        {
            { {-1, 0, 1}, { 0,-1, 1}, {-1,-1, 1} },  // v0
            { { 1, 0, 1}, { 0,-1, 1}, { 1,-1, 1} },  // v1
            { { 1, 0, 1}, { 0, 1, 1}, { 1, 1, 1} },  // v2
            { {-1, 0, 1}, { 0, 1, 1}, {-1, 1, 1} },  // v3
        },
        // NegativeZ (Back/North face, -Z) — vertices: 0=bottom-right, 1=bottom-left, 2=top-left, 3=top-right
        {
            { { 1, 0,-1}, { 0,-1,-1}, { 1,-1,-1} },  // v0
            { {-1, 0,-1}, { 0,-1,-1}, {-1,-1,-1} },  // v1
            { {-1, 0,-1}, { 0, 1,-1}, {-1, 1,-1} },  // v2
            { { 1, 0,-1}, { 0, 1,-1}, { 1, 1,-1} },  // v3
        },
        // PositiveX (Right/East face, +X) — vertices: 0=bottom-front, 1=bottom-back, 2=top-back, 3=top-front
        {
            { { 1, 0, 1}, { 1,-1, 0}, { 1,-1, 1} },  // v0
            { { 1, 0,-1}, { 1,-1, 0}, { 1,-1,-1} },  // v1
            { { 1, 0,-1}, { 1, 1, 0}, { 1, 1,-1} },  // v2
            { { 1, 0, 1}, { 1, 1, 0}, { 1, 1, 1} },  // v3
        },
        // NegativeX (Left/West face, -X) — vertices: 0=bottom-back, 1=bottom-front, 2=top-front, 3=top-back
        {
            { {-1, 0,-1}, {-1,-1, 0}, {-1,-1,-1} },  // v0
            { {-1, 0, 1}, {-1,-1, 0}, {-1,-1, 1} },  // v1
            { {-1, 0, 1}, {-1, 1, 0}, {-1, 1, 1} },  // v2
            { {-1, 0,-1}, {-1, 1, 0}, {-1, 1,-1} },  // v3
        },
    };

    // Minecraft-style per-vertex AO calculation
    // Uses the exact Minecraft values: solid blocks = 0.2 shade, non-solid = 1.0
    // Formula: vertex_ao = (edge1 + edge2 + corner + center) * 0.25
    // Corner rule: if both edges are solid, corner is forced solid (prevents diagonal light leak)
    float Mesher::CalculateVertexAO(const Game::IBlockAccess& /*blocks*/, int worldX, int worldY, int worldZ,
                                    BlockFace face, int vertexIndex) {
        if (!m_config.enableAmbientOcclusion) {
            return 1.0f;
        }

        const auto& neighbors = AO_NEIGHBORS[static_cast<int>(face)][vertexIndex];

        // Use the pre-built opaque cache instead of GetBlock + IsBlockOpaque per sample.
        // This eliminates 12 block lookups + 12 registry lookups per face (3 per vertex × 4 vertices).
        bool edge1Solid = GetCachedOpaque(worldX + neighbors.edge1.x, worldY + neighbors.edge1.y, worldZ + neighbors.edge1.z);
        bool edge2Solid = GetCachedOpaque(worldX + neighbors.edge2.x, worldY + neighbors.edge2.y, worldZ + neighbors.edge2.z);

        float edge1Shade = edge1Solid ? 0.2f : 1.0f;
        float edge2Shade = edge2Solid ? 0.2f : 1.0f;

        // Corner rule: if both edges are solid, corner is forced solid (no diagonal light leak)
        float cornerShade;
        if (edge1Solid && edge2Solid) {
            cornerShade = 0.2f;
        } else {
            bool cornerSolid = GetCachedOpaque(worldX + neighbors.corner.x, worldY + neighbors.corner.y, worldZ + neighbors.corner.z);
            cornerShade = cornerSolid ? 0.2f : 1.0f;
        }

        float centerShade = 1.0f;
        return (edge1Shade + edge2Shade + cornerShade + centerShade) * 0.25f;
    }

    // Render layer classification — uses thread-local cache when available,
    // falls back to registry for initial population or external callers.
    RenderLayer ClassifyBlock(Game::BlockID blockId) {
        auto idx = static_cast<uint16_t>(blockId);
        if (Mesher::s_blockPropsCacheValid && idx < Mesher::BLOCK_ID_COUNT) {
            return Mesher::s_blockPropsCache[idx].renderLayer;
        }
        switch (Game::BlockRegistry::Get(blockId).renderLayer) {
            case Game::RenderLayer::Cutout:       return RenderLayer::Cutout;
            case Game::RenderLayer::Translucent:  return RenderLayer::Translucent;
            default:                              return RenderLayer::Opaque;
        }
    }

    bool IsBlockOpaque(Game::BlockID blockId) {
        auto idx = static_cast<uint16_t>(blockId);
        if (Mesher::s_blockPropsCacheValid && idx < Mesher::BLOCK_ID_COUNT) {
            return Mesher::s_blockPropsCache[idx].isOpaque;
        }
        return Game::BlockRegistry::Get(blockId).opaque;
    }

    bool IsBlockTranslucent(Game::BlockID blockId) {
        return ClassifyBlock(blockId) == RenderLayer::Translucent;
    }

} // namespace Render