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
        // The `age` of a melon/pumpkin stem, for the StemAge tint. Reads the
        // state definition rather than assuming state index == age: that
        // happens to hold today (stems declare `age` as their only property)
        // but would silently produce wrong colours the moment one gained a
        // second property.
        int StemAgeOf(Game::BlockID id, Game::BlockStateIndex stateIndex) {
            const std::string_view v =
                Game::BlockRegistry::GetStateDefinition(id).ValueOf(stateIndex, "age");
            int n = 0;
            for (char c : v) {
                if (c < '0' || c > '9') return 0;
                n = n * 10 + (c - '0');
            }
            return n;
        }

        class CacheBlockAccess final : public Game::IBlockAccess {
        public:
            CacheBlockAccess(const Game::BlockID (&cache)[18][18][18],
                             const Game::BlockStateIndex (&states)[18][18][18],
                             const bool (&water)[18][18][18],
                             bool statesAllZero,
                             int baseX, int baseY, int baseZ)
                : m_cache(cache), m_states(states), m_water(water),
                  m_statesAllZero(statesAllZero),
                  m_baseX(baseX), m_baseY(baseY), m_baseZ(baseZ) {}

            Game::BlockID GetBlock(int worldX, int worldY, int worldZ) const override {
                const int lx = worldX - m_baseX;
                const int ly = worldY - m_baseY;
                const int lz = worldZ - m_baseZ;
                if (lx < -1 || lx > 16 || ly < -1 || ly > 16 || lz < -1 || lz > 16) {
                    return Game::BlockID::Air;
                }
                return m_cache[ly + 1][lz + 1][lx + 1];
            }

            // Was inherited (always 0) until waterlogging made a neighbour's
            // state decide whether a fluid face is drawn. The state cache now
            // spans the same 18^3 halo as the block cache, so this is the same
            // bounds check and the same array read.
            Game::BlockState GetBlockState(int worldX, int worldY, int worldZ) const override {
                const int lx = worldX - m_baseX;
                const int ly = worldY - m_baseY;
                const int lz = worldZ - m_baseZ;
                if (lx < -1 || lx > 16 || ly < -1 || ly > 16 || lz < -1 || lz > 16) {
                    return Game::BlockState{};
                }
                // The cache still keeps block and index in separate planes, so
                // this is the one place that recombines them. `m_statesAllZero`
                // cannot short-circuit it any more: index 0 is a real state of
                // whatever block is here, not "no state".
                const Game::BlockID id = m_cache[ly + 1][lz + 1][lx + 1];
                const Game::BlockStateIndex st =
                    m_statesAllZero ? 0 : m_states[ly + 1][lz + 1][lx + 1];
                return Game::BlockStates::FromIndex(id, st);
            }

            // Straight off the derived cache rather than through the base
            // class's GetBlock + GetBlockState + registry composition. This is
            // the single hottest query in fluid meshing.
            bool ContainsWater(int worldX, int worldY, int worldZ) const override {
                const int lx = worldX - m_baseX;
                const int ly = worldY - m_baseY;
                const int lz = worldZ - m_baseZ;
                if (lx < -1 || lx > 16 || ly < -1 || ly > 16 || lz < -1 || lz > 16) {
                    return false;
                }
                return m_water[ly + 1][lz + 1][lx + 1];
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
                if (ContainsWater(worldX, worldY, worldZ)) return true;
                return GetBlock(worldX, worldY, worldZ) == Game::BlockID::Lava;
            }

            bool IsValidPosition(int, int worldY, int) const override {
                return worldY >= Config::MinY && worldY <= Config::MaxY;
            }

        private:
            const Game::BlockID (&m_cache)[18][18][18];
            const Game::BlockStateIndex (&m_states)[18][18][18];
            const bool (&m_water)[18][18][18];
            bool m_statesAllZero;
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
        std::memset(m_stateCache, 0, sizeof(m_stateCache));
        std::memset(m_waterCache, 0, sizeof(m_waterCache));
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
            // (a single box spanning 0..1 on every axis) participate in face
            // culling. Slab tops/bottoms not culling the cube above/below them
            // is a minor hidden-face overdraw that we accept until a proper
            // per-face occlusion mask is added.
            //
            // Asked of the box UNION, not its bounds: a stair is a bottom slab
            // plus a step, and between them they touch every face, so on bounds
            // alone every stair would occlude and a wall behind one would lose
            // the face the stair's open half is meant to show through.
            //
            // Asked of EVERY state, not just state 0. A 90° turn about the cell
            // centre maps the unit cube onto itself, so rotation alone cannot
            // change the answer — but a property that changes how much of the
            // cell is filled can, and a slab's `type` is exactly that.
            //
            // Slabs now genuinely DO disagree with themselves: `type=double`
            // fills the cell while top and bottom do not, all under one
            // BlockID. Before the synthetic ids collapsed, the three halves
            // were separate blocks and this loop only ever confirmed. It
            // discovers now, and DeriveOpaqueCache reads the per-voxel state
            // for anything it flags — without which a double slab would mesh
            // as a half slab and leak faces.
            //
            // No 256 clamp: the state index is 16-bit, and 30 blocks exceed 256
            // states. Clamping here would silently skip the tail of every one
            // of them.
            const uint16_t stateCount = Game::BlockRegistry::GetStateCount(blockId);
            const bool fullCube =
                Game::BlockRegistry::GetBlockShapeSet(
                    Game::BlockStates::FromIndex(blockId, 0)).IsFullCube();
            bool variesByState = false;
            for (uint16_t s = 1; s < stateCount; ++s) {
                if (Game::BlockRegistry::GetBlockShapeSet(
                        Game::BlockStates::FromIndex(blockId, s)).IsFullCube() != fullCube) {
                    variesByState = true;
                    break;
                }
            }
            s_blockPropsCache[i].occlusionVariesByState = variesByState;
            // BE-flagged blocks (chest, shulker, sign, banner, …) draw their
            // geometry via the BlockEntityRenderer, NOT via the chunk mesh.
            // From the chunk-mesh's POV the cell is empty even if `block.opaque`
            // is true and the model shape defaults to a full cube. Marking
            // them as non-occluding here prevents the neighbour-face cull
            // from punching visible holes in adjacent walls behind a chest.
            const bool beFlagged = Game::BlockEntityTypes::HasBlockEntity(blockId);
            s_blockPropsCache[i].opaqueMaterial = block.opaque && !beFlagged;
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
                } else if (is("redstone_wire")) {
                    // MC BlockColors registers RedStoneWireBlock.getColorForPower;
                    // RedStoneWireBlock.java:453-461 builds the table as
                    //   red   = power*0.6 + (power > 0 ? 0.4 : 0.3)
                    //   green = clamp(power*power*0.7 - 0.5, 0, 1)
                    //   blue  = clamp(power*power*0.6 - 0.7, 0, 1)
                    // At power 0 that is (0.3, 0, 0) -> 0x4D0000, the dark red
                    // of unpowered dust. A CONSTANT is exactly right here: there
                    // is no redstone power simulation, so every wire is at 0.
                    // When power arrives this becomes a per-state resolver, the
                    // same shape as the stem tint below.
                    //
                    // The dust texture is greyscale and relies entirely on this
                    // tint — untinted it renders white.
                    p.tintSource = TS::Constant;
                    p.tintConstant = 0x4D0000;
                } else if (is("melon_stem") || is("pumpkin_stem")) {
                    // BlockColors.java:54-57 — a growing stem fades from green
                    // to the attached stem's yellow as it ages:
                    //   ARGB.color(age * 32, 255 - age * 8, age * 4)
                    // MC also calls addColoringState(StemBlock.AGE, …), which
                    // is what tells it to re-bake per state; here the per-state
                    // part is the stateIndex threaded into AddBlockFace.
                    //
                    // There is only ONE stem texture (melon_stem.png), greyscale
                    // — every stage's colour comes from this tint, so without it
                    // a whole field of stems renders identically grey-green.
                    p.tintSource = TS::StemAge;
                }
            }
            switch (block.renderLayer) {
                case Game::RenderLayer::Cutout:      s_blockPropsCache[i].renderLayer = RenderLayer::Cutout; break;
                case Game::RenderLayer::Translucent:  s_blockPropsCache[i].renderLayer = RenderLayer::Translucent; break;
                default:                              s_blockPropsCache[i].renderLayer = RenderLayer::Opaque; break;
            }
        }

        // Report which blocks disagree with themselves about occlusion. Expect
        // none today; expect the slab family the moment `type` becomes a
        // property. Logged rather than assumed so that change announces itself
        // instead of showing up later as slabs leaking faces.
        {
            size_t varying = 0;
            for (size_t i = 0; i < BLOCK_ID_COUNT; ++i) {
                if (s_blockPropsCache[i].occlusionVariesByState) ++varying;
            }
            if (varying > 0) {
                Log::Info("Mesher: %zu block(s) occlude differently per state - "
                          "their opacity is resolved per voxel", varying);
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

        // States over the FULL 18^3, halo included: a waterlogged neighbour
        // culls the fluid face it shares with this cell, so the halo's state
        // is read during meshing just as its block id is.
        m_stateCacheAllZero = true;
        for (int ly = -1; ly <= 16; ++ly) {
            for (int lz = -1; lz <= 16; ++lz) {
                for (int lx = -1; lx <= 16; ++lx) {
                    // The cache keeps the within-block index, not the global
                    // id — it is paired with m_blockCache and read back through
                    // CacheBlockAccess, which recombines them.
                    const Game::BlockStateIndex st =
                        blocks.GetBlockState(m_sectionBaseWorldX + lx,
                                             m_sectionBaseWorldY + ly,
                                             m_sectionBaseWorldZ + lz).Index();
                    m_stateCache[ly + 1][lz + 1][lx + 1] = st;
                    if (st != 0) m_stateCacheAllZero = false;
                }
            }
        }
    }

    void Mesher::FillBlockCacheFromRegion(const Client::Render::RegionSnapshot& region,
                                          Game::Math::ChunkPos chunkPos, int sectionY) {
        PROFILE_ZONE;
        m_sectionBaseWorldX = chunkPos.x * 16;
        m_sectionBaseWorldY = sectionY * 16 + Config::MinY;
        m_sectionBaseWorldZ = chunkPos.z * 16;

        m_biomeSource = &region;
        m_biomeAccess = nullptr;

        const Client::Render::SectionCopy* centre = region.Centre();

        // Interior 16^3, walked as one linear pass over the centre section's
        // container. Index i in the container is the same [y][z][x] ordering
        // the cache uses (Math::LocalIndex), so the coordinates come out of i
        // by shifting rather than from a triple loop, and each voxel costs one
        // container read plus one Unpack for the block AND its state together.
        //
        // MC reads its region per voxel in exactly this way
        // (RenderSectionRegion.getBlockState); there is no contiguous array to
        // memcpy from once the storage is paletted, and that is the trade the
        // palette makes everywhere else too.
        m_stateCacheAllZero = true;
        for (int i = 0; i < 4096; ++i) {
            const int lx = i & 15;
            const int lz = (i >> 4) & 15;
            const int ly = (i >> 8) & 15;

            const Game::BlockState ref = Game::BlockState::FromRawId(
                centre ? centre->GetStateId(static_cast<size_t>(i))
                       : Game::BlockState{}.RawId());

            m_blockCache[ly + 1][lz + 1][lx + 1] = ref.Block();
            m_stateCache[ly + 1][lz + 1][lx + 1] = ref.Index();
            if (ref.Index() != 0) m_stateCacheAllZero = false;
        }

        // The halo: faces, edges AND corners, every one a real block from the
        // neighbouring section copies rather than a clamped approximation.
        //
        // Block AND state together, via StateIdAtLocal's single packed read —
        // the halo's state matters now that a waterlogged neighbour decides
        // whether the fluid face they share is drawn.
        for (int ly = -1; ly <= 16; ++ly) {
            for (int lz = -1; lz <= 16; ++lz) {
                for (int lx = -1; lx <= 16; ++lx) {
                    const bool interior = (lx >= 0 && lx < 16) &&
                                          (ly >= 0 && ly < 16) &&
                                          (lz >= 0 && lz < 16);
                    if (interior) continue;
                    const Game::BlockState ref =
                        Game::BlockState::FromRawId(region.StateIdAtLocal(lx, ly, lz));
                    m_blockCache[ly + 1][lz + 1][lx + 1] = ref.Block();
                    m_stateCache[ly + 1][lz + 1][lx + 1] = ref.Index();
                    if (ref.Index() != 0) m_stateCacheAllZero = false;
                }
            }
        }
    }

    void Mesher::DeriveOpaqueCache() {
        PROFILE_ZONE;
        // One pass over the 18^3 block cache: opacity via the per-thread props
        // table. The table answers for the block's default state, which is the
        // whole answer only when every state of that block occludes alike.
        //
        // `occlusionVariesByState` is false for every block today, so this is
        // one predictable branch and the same single array read it always was.
        // It exists because occlusion is NOT a per-BlockID property in general:
        // a slab occludes at `type=double` and does not at `type=bottom`, and
        // once those stop being separate BlockIDs this table alone would make
        // every double slab either leak faces or eat its neighbours'. Same
        // shape as DeriveWaterCache below, which already reads both planes.
        const Game::BlockID* src = &m_blockCache[0][0][0];
        const Game::BlockStateIndex* st = &m_stateCache[0][0][0];
        bool* dst = &m_opaqueCache[0][0][0];
        for (size_t i = 0; i < 18 * 18 * 18; ++i) {
            const auto& props = s_blockPropsCache[static_cast<uint16_t>(src[i])];
            if (!props.occlusionVariesByState) {
                dst[i] = props.isOpaque;
                continue;
            }
            dst[i] = props.opaqueMaterial &&
                     Game::BlockRegistry::GetBlockShapeSet(
                         Game::BlockStates::FromIndex(src[i], st[i])).IsFullCube();
        }
    }

    void Mesher::DeriveWaterCache() {
        PROFILE_ZONE;
        // One pass over the 18^3 block+state caches, mirroring MC's
        // BlockState.getFluidState().is(WATER) for every cell in the region.
        //
        // The fast path below is for a region whose states are all literally
        // ZERO, which now means "every block here has no properties at all" —
        // ordinary stone-and-dirt terrain. It is no longer the same thing as
        // "everything is at its default": most blocks default to a non-zero
        // index, so a section containing one slab takes the slow path. Still
        // correct either way; it just fires less often than it reads like it
        // should.
        const Game::BlockID* src = &m_blockCache[0][0][0];
        const Game::BlockStateIndex* st = &m_stateCache[0][0][0];
        bool* dst = &m_waterCache[0][0][0];
        if (m_stateCacheAllZero) {
            for (size_t i = 0; i < 18 * 18 * 18; ++i) {
                dst[i] = Game::BlockRegistry::ContainsWater(
                    Game::BlockStates::FromIndex(src[i], 0));
            }
            return;
        }
        for (size_t i = 0; i < 18 * 18 * 18; ++i) {
            dst[i] = Game::BlockRegistry::ContainsWater(
                Game::BlockStates::FromIndex(src[i], st[i]));
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

    void Mesher::BuildSectionMesh(const Client::Render::RegionSnapshot& region,
                                  Game::Math::ChunkPos chunkPos, int sectionY, SectionMesh& outMesh) {
        EnsureBlockPropsCache();
        FillBlockCacheFromRegion(region, chunkPos, sectionY);
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
        // Same idea for "this cell's fluid state is water", which the fluid
        // mesher asks of every voxel and all six of its neighbours.
        DeriveWaterCache();

        // Adapter handed to downstream IBlockAccess consumers (fluid builder);
        // every read resolves to a cache array access.
        CacheBlockAccess cacheAccess(m_blockCache, m_stateCache, m_waterCache,
                                     m_stateCacheAllZero,
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
                             int sectionY, Game::BlockID blockId, Game::BlockStateIndex stateIndex,
                             SectionMesh& mesh) {

        int worldX = chunkPos.x * 16 + localX;
        int worldZ = chunkPos.z * 16 + localZ;
        // blockId passed from caller — avoids redundant GetBlock() virtual call

        // Fluids. MC SectionCompiler.java:64-69 runs the liquid renderer for
        // ANY block whose getFluidState() is non-empty and then, separately,
        // renders the block model — the two are independent passes over the
        // same voxel, which is the whole reason a waterlogged fence can show
        // both its posts and the water around them.
        //
        // The fluid is emitted first, matching MC's order.
        const Game::BlockState state = Game::BlockStates::FromIndex(blockId, stateIndex);
        const bool holdsWater = Game::BlockRegistry::ContainsWater(state);
        if (holdsWater || blockId == Game::BlockID::Lava) {
            if (m_fluidBuilder) {
                m_fluidBuilder->BuildFluidBlock(blocks, chunkPos, worldX, worldY, worldZ, mesh);
                m_lastStats.facesGenerated++;
            }
            // MC LiquidBlock.getRenderShape() is INVISIBLE, so a plain water or
            // lava cell contributes no model geometry — only the fluid above.
            // Everything else falls through and draws its model as usual.
            if (blockId == Game::BlockID::Water || blockId == Game::BlockID::Lava) {
                return;
            }
        }

        // Model is keyed on (block, state) — the blockstate JSON maps each
        // state to its own, possibly pre-rotated, model. MC's equivalent lookup
        // is BlockModelShaper.getBlockModel(BlockState).
        const Game::BlockModel& model = Game::BlockRegistry::GetBlockModel(state);
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
                AddBlockFace(blocks, model, element, faceDir, faceDef, worldPos, faceNormal, blockId, stateIndex, worldX, worldY, worldZ, blockLayer, mesh);
                m_lastStats.facesGenerated++;
            }
        }
    }

    void Mesher::AddBlockFace(const Game::IBlockAccess& blocks,
                             const Game::BlockModel& model, const Game::Element& element,
                             Game::FaceDir faceDir, const Game::FaceDef& faceDef,
                             glm::vec3 blockPos, glm::vec3 faceNormal, Game::BlockID blockId,
                             Game::BlockStateIndex stateIndex,
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
                case TS::StemAge: {
                    // BlockColors.java:54-57, verbatim:
                    //   ARGB.color(age * 32, 255 - age * 8, age * 4)
                    // age 0 = (0, 255, 0) bright green; age 7 = (224, 199, 28),
                    // which is exactly the attached stem's constant, so a stem
                    // that matures and attaches does not visibly change colour.
                    const int age = StemAgeOf(blockId, stateIndex);
                    tintColor = glm::vec4(static_cast<float>(age * 32)       / 255.0f,
                                          static_cast<float>(255 - age * 8)  / 255.0f,
                                          static_cast<float>(age * 4)        / 255.0f,
                                          1.0f);
                    break;
                }
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

        // Where each vertex sits within its CELL, captured before the element
        // rotation and the block offset move it. This is what AO is sampled
        // against (MC does the same: AmbientOcclusionFace runs on the quad's
        // pre-transform shape), and both of those transforms would otherwise
        // push a vertex outside [0,1] and skew the blend.
        glm::vec3 aoLocal[4];
        for (int v = 0; v < 4; ++v) aoLocal[v] = faceVerts[v].pos - blockPos;

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
        float aoShades[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        if (useAO) {
            ComputeFaceAO(blocks, worldX, worldY, worldZ, blockFace, aoLocal, aoShades);
        }
        for (int v = 0; v < 4; ++v) {
            float finalShade = aoShades[v] * directionalShade;
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
            // The region covers the whole 3x3x3 neighbourhood, so the blend
            // margin resolves against real neighbour biomes rather than against
            // a clamped edge of a per-job margin grid.
            return m_biomeSource->BiomeAtLocal(worldX - m_sectionBaseWorldX,
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
        // Values live in Game::DirectionalShade (BlockModel.hpp) so the chunk
        // mesher and the item-model builder cannot drift apart — a dropped
        // block has to be shaded identically to the one it came from.
        switch (face) {
            case BlockFace::PositiveY: return Game::DirectionalShade(Game::FaceDir::Up);
            case BlockFace::NegativeY: return Game::DirectionalShade(Game::FaceDir::Down);
            case BlockFace::PositiveZ: return Game::DirectionalShade(Game::FaceDir::South);
            case BlockFace::NegativeZ: return Game::DirectionalShade(Game::FaceDir::North);
            case BlockFace::PositiveX: return Game::DirectionalShade(Game::FaceDir::East);
            case BlockFace::NegativeX: return Game::DirectionalShade(Game::FaceDir::West);
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

    void Mesher::ComputeFaceAO(const Game::IBlockAccess& blocks,
                               int worldX, int worldY, int worldZ,
                               BlockFace face, const glm::vec3 (&localPos)[4],
                               float (&outAO)[4]) {
        if (!m_config.enableAmbientOcclusion) {
            outAO[0] = outAO[1] = outAO[2] = outAO[3] = 1.0f;
            return;
        }

        // The face's two in-plane axes. The normal axis is whichever component
        // of the face normal is non-zero.
        const glm::vec3 n = GetFaceNormal(face);
        const int nAxis = (n.x != 0.0f) ? 0 : ((n.y != 0.0f) ? 1 : 2);
        const int a1 = (nAxis + 1) % 3;
        const int a2 = (nAxis + 2) % 3;

        // Each corner's value, and WHICH corner of the cell's face it is.
        // The side is read straight off AO_NEIGHBORS' own corner offset, so the
        // weights cannot drift from the values they are weighting.
        float cornerAO[4];
        float cornerS[4], cornerT[4];
        for (int k = 0; k < 4; ++k) {
            cornerAO[k] = CalculateVertexAO(blocks, worldX, worldY, worldZ, face, k);
            const glm::ivec3& c = AO_NEIGHBORS[static_cast<int>(face)][k].corner;
            cornerS[k] = (c[a1] > 0) ? 1.0f : 0.0f;
            cornerT[k] = (c[a2] > 0) ? 1.0f : 0.0f;
        }

        for (int v = 0; v < 4; ++v) {
            const float s = std::clamp(localPos[v][a1], 0.0f, 1.0f);
            const float t = std::clamp(localPos[v][a2], 0.0f, 1.0f);
            float ao = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const float ws = (cornerS[k] > 0.5f) ? s : (1.0f - s);
                const float wt = (cornerT[k] > 0.5f) ? t : (1.0f - t);
                ao += cornerAO[k] * ws * wt;
            }
            outAO[v] = ao;
        }
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