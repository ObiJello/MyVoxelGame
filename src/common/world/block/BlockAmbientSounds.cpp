// File: src/common/world/block/BlockAmbientSounds.cpp
#include "common/world/block/BlockAmbientSounds.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace Game {

    namespace {

        // ── Block tags the conditions read ──────────────────────────────────
        //
        // Resolved once per tag into a per-BlockID table on first use (the
        // data pack is read lazily by DataTags; animateTick is client-only and
        // runs long after startup).
        class TagTable {
        public:
            explicit TagTable(const char* tag) : m_tag(tag) {}
            bool Has(BlockID id) {
                if (m_table.empty()) {
                    m_table.assign(BlockRegistry::Size, 0);
                    for (size_t i = 0; i < BlockRegistry::Size; ++i) {
                        const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                        if (slug.empty()) continue;
                        const auto& tags = DataTags::TagsFor(DataTags::Registry::Block, slug);
                        m_table[i] = std::find(tags.begin(), tags.end(), m_tag) != tags.end() ? 1 : 0;
                    }
                }
                const size_t i = static_cast<size_t>(id);
                return i < m_table.size() && m_table[i] != 0;
            }
        private:
            std::string          m_tag;
            std::vector<uint8_t> m_table;
        };

        TagTable& PaleOakLogs()         { static TagTable t("#minecraft:pale_oak_logs"); return t; }
        TagTable& DesertSandTriggers()  { static TagTable t("#minecraft:triggers_ambient_desert_sand_block_sounds"); return t; }
        TagTable& DesertDryVegetationTriggers() {
            static TagTable t("#minecraft:triggers_ambient_desert_dry_vegetation_block_sounds");
            return t;
        }
        TagTable& DriedGhastTriggers()  { static TagTable t("#minecraft:triggers_ambient_dried_ghast_block_sounds"); return t; }
        TagTable& Terracotta()          { static TagTable t("#minecraft:terracotta"); return t; }

        BlockState StateAt(const IBlockAccess& b, const glm::ivec3& p) { return b.GetBlockState(p.x, p.y, p.z); }
        BlockID    BlockAt(const IBlockAccess& b, const glm::ivec3& p) { return b.GetBlock(p.x, p.y, p.z); }

        glm::dvec3 Centre(const glm::ivec3& p) { return glm::dvec3(p) + glm::dvec3(0.5); }
        glm::dvec3 Corner(const glm::ivec3& p) { return glm::dvec3(p); }

        bool IsTrue(BlockState s, std::string_view prop) { return s.GetValueByName(prop) == "true"; }

        // The DAY timeline's night window (data/minecraft/timeline/day.json):
        // audio/firefly_bush_sounds and gameplay/creaking_active are both
        // true from 12600 to 23401 in the Overworld.
        bool IsTimelineNight(const EntityLevel& level) {
            if (level.Dimension() != DimensionId::Overworld) return false;
            const int64_t t = ((level.GetDayTime() % 24000) + 24000) % 24000;
            return t >= 12600 && t < 23401;
        }

        // MC getHeight(heightmap, pos) at the cell's column, scanned from the
        // top. `motionBlockingNoLeaves` picks MOTION_BLOCKING_NO_LEAVES (a
        // solid or fluid block that is not leaves) over WORLD_SURFACE (any
        // non-air block). Returns the Y above the top such block.
        int ColumnHeight(const IBlockAccess& blocks, int x, int z, bool motionBlockingNoLeaves) {
            for (int y = World::MAX_Y; y >= World::MIN_Y; --y) {
                const BlockID id = blocks.GetBlock(x, y, z);
                if (id == BlockID::Air) continue;
                if (!motionBlockingNoLeaves) return y + 1;
                if (!(blocks.IsBlockSolid(x, y, z) || blocks.IsBlockFluid(x, y, z))) continue;
                const std::string& slug = BlockRegistry::Get(id).registrySlug;
                if (slug.size() > 7 && slug.compare(slug.size() - 7, 7, "_leaves") == 0) continue;
                return y + 1;
            }
            return World::MIN_Y;
        }

        // ── MC AmbientDesertBlockSoundsPlayer ───────────────────────────────

        bool ColumnContainsTriggeringBlock(const IBlockAccess& blocks, glm::ivec3 p) {
            const int surfaceY = ColumnHeight(blocks, p.x, p.z, false) - 1;
            if (std::abs(surfaceY - p.y) > 5) {
                p.y += 6;
                BlockState above = StateAt(blocks, p);
                --p.y;
                for (int i = 0; i < 10; ++i) {
                    const BlockState current = StateAt(blocks, p);
                    if (above.Block() == BlockID::Air && DesertSandTriggers().Has(current.Block())) return true;
                    above = current;
                    --p.y;
                }
                return false;
            }
            const bool airAbove = blocks.GetBlock(p.x, surfaceY + 1, p.z) == BlockID::Air;
            return airAbove && DesertSandTriggers().Has(blocks.GetBlock(p.x, surfaceY, p.z));
        }

        bool ShouldPlayAmbientSandSound(const IBlockAccess& blocks, const glm::ivec3& pos) {
            // Three of the four columns eight blocks out must be desert too.
            static constexpr glm::ivec3 kDirs[4] = {{0, 0, -1}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}};
            int matching = 0;
            int checked = 0;
            for (const glm::ivec3& d : kDirs) {
                if (ColumnContainsTriggeringBlock(blocks, pos + d * 8) && matching++ >= 3) return true;
                ++checked;
                if ((4 - checked) + matching < 3) return false;
            }
            return false;
        }

        bool ShouldPlayDesertDryVegetationBlockSounds(const IBlockAccess& blocks, const glm::ivec3& below) {
            return DesertDryVegetationTriggers().Has(BlockAt(blocks, below)) &&
                   DesertDryVegetationTriggers().Has(BlockAt(blocks, below - glm::ivec3(0, 1, 0)));
        }

        // ── The sound-only animateTicks ─────────────────────────────────────

        // MC BaseFireBlock.animateTick:52 — one in 24.
        void FireSound(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            if (random.NextInt(24) == 0) {
                const float volume = 1.0f + random.NextFloat();
                level.PlayLocalSound(Centre(pos), SoundEvents::FIRE_AMBIENT, SoundSource::Blocks,
                                     volume, random.NextFloat() * 0.7f + 0.3f, false);
            }
        }

        // MC CampfireBlock.animateTick:138 — lit, one in 10.
        void CampfireSound(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (!IsTrue(state, "lit")) return;
            if (random.NextInt(10) == 0) {
                const float volume = 0.5f + random.NextFloat();
                level.PlayLocalSound(Centre(pos), SoundEvents::CAMPFIRE_CRACKLE, SoundSource::Blocks,
                                     volume, random.NextFloat() * 0.7f + 0.6f, false);
            }
        }

        // MC FurnaceBlock / SmokerBlock / BlastFurnaceBlock.animateTick: lit,
        // nextDouble() < 0.1, at the bottom centre.
        template <const char* const* kEvent>
        void CookerSound(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (!IsTrue(state, "lit")) return;
            if (random.NextDouble() < 0.1) {
                level.PlayLocalSound(glm::dvec3(pos.x + 0.5, pos.y, pos.z + 0.5), *kEvent, SoundSource::Blocks,
                                     1.0f, 1.0f, false);
            }
        }
        const char* const kFurnaceCrackle = SoundEvents::FURNACE_FIRE_CRACKLE;
        const char* const kSmokerSmoke    = SoundEvents::SMOKER_SMOKE;
        const char* const kBlastCrackle   = SoundEvents::BLASTFURNACE_FIRE_CRACKLE;

        // MC AbstractCandleBlock.animateTick → addParticlesAndSound per flame:
        // chance < 0.17 of the flame's roll plays CANDLE_AMBIENT. One flame
        // per candle (CandleBlock.getParticleOffsets), one on a candle cake.
        void CandleSound(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (!IsTrue(state, "lit")) return;
            int flames = 1;
            const std::string_view candles = state.GetValueByName("candles");
            if (!candles.empty()) flames = std::clamp(candles.front() - '0', 1, 4);
            for (int i = 0; i < flames; ++i) {
                if (random.NextFloat() < 0.17f) {
                    const float volume = 1.0f + random.NextFloat();
                    level.PlayLocalSound(Centre(pos), SoundEvents::CANDLE_AMBIENT, SoundSource::Blocks,
                                         volume, random.NextFloat() * 0.7f + 0.3f, false);
                }
            }
        }

        // MC RespawnAnchorBlock.animateTick:155 — charged, one in 100.
        void RespawnAnchorSound(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            const std::string_view charges = state.GetValueByName("charges");
            if (charges.empty() || charges == "0") return;
            if (random.NextInt(100) == 0) {
                level.PlayLocalSound(Centre(pos), SoundEvents::RESPAWN_ANCHOR_AMBIENT, SoundSource::Blocks,
                                     1.0f, 1.0f, false);
            }
        }

        // MC BubbleColumnBlock.animateTick:115/121 — one in 200, whirlpool
        // over a drag-down column (magma), upwards otherwise, at the corner.
        void BubbleColumnSound(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (random.NextInt(200) != 0) return;
            const float volume = 0.2f + random.NextFloat() * 0.2f;
            const float pitch = 0.9f + random.NextFloat() * 0.15f;
            level.PlayLocalSound(Corner(pos), IsTrue(state, "drag") ? SoundEvents::BUBBLE_COLUMN_WHIRLPOOL_AMBIENT
                                                                    : SoundEvents::BUBBLE_COLUMN_UPWARDS_AMBIENT,
                                 SoundSource::Blocks, volume, pitch, false);
        }

        // MC NetherPortalBlock.animateTick:188 — one in 100.
        void NetherPortalSound(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            if (random.NextInt(100) == 0) {
                level.PlayLocalSound(Centre(pos), SoundEvents::PORTAL_AMBIENT, SoundSource::Blocks, 0.5f,
                                     random.NextFloat() * 0.4f + 0.8f, false);
            }
        }

        // MC FireflyBushBlock.animateTick:28 — one in 30, the DAY timeline's
        // night, and open to the sky (MOTION_BLOCKING_NO_LEAVES).
        void FireflyBushSound(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            if (random.NextInt(30) != 0 || !IsTimelineNight(level)) return;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks || ColumnHeight(*blocks, pos.x, pos.z, true) > pos.y) return;
            level.PlayLocalSound(Centre(pos), SoundEvents::FIREFLY_BUSH_IDLE, SoundSource::Ambient, 1.0f, 1.0f, false);
        }

        // MC HangingMossBlock.animateTick:37 — one in 500, under pale oak.
        void PaleHangingMossSound(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            if (random.NextInt(500) != 0) return;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            const BlockID above = BlockAt(*blocks, pos + glm::ivec3(0, 1, 0));
            if (PaleOakLogs().Has(above) || above == BlockID::PaleOakLeaves) {
                level.PlayLocalSound(Corner(pos), SoundEvents::PALE_HANGING_MOSS_IDLE, SoundSource::Ambient,
                                     1.0f, 1.0f, false);
            }
        }

        // MC EyeblossomBlock.animateTick:42 — open ones, one in 700, on pale moss.
        void OpenEyeblossomSound(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            if (random.NextInt(700) != 0) return;
            const IBlockAccess* blocks = level.Blocks();
            if (blocks && BlockAt(*blocks, pos - glm::ivec3(0, 1, 0)) == BlockID::PaleMossBlock) {
                level.PlayLocalSound(Corner(pos), SoundEvents::EYEBLOSSOM_IDLE, SoundSource::Ambient, 1.0f, 1.0f, false);
            }
        }

        // MC CreakingHeartBlock.animateTick:59 — creaking_active (the night),
        // not uprooted, one in 16, pale oak logs on every side.
        void CreakingHeartSound(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (!IsTimelineNight(level)) return;
            if (state.GetValueByName("creaking_heart_state") == "uprooted") return;
            if (random.NextInt(16) != 0) return;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            static constexpr glm::ivec3 kSides[6] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
            for (const glm::ivec3& d : kSides) {
                if (!PaleOakLogs().Has(BlockAt(*blocks, pos + d))) return;
            }
            level.PlayLocalSound(Corner(pos), SoundEvents::CREAKING_HEART_IDLE, SoundSource::Blocks, 1.0f, 1.0f, false);
        }

        // MC DriedGhastBlock.animateTick:119/128 — dry on soul sand, or wet.
        void DriedGhastSound(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            if (!IsTrue(state, "waterlogged")) {
                if (random.NextInt(40) == 0 && DriedGhastTriggers().Has(BlockAt(*blocks, pos - glm::ivec3(0, 1, 0)))) {
                    level.PlayLocalSound(Centre(pos), SoundEvents::DRIED_GHAST_AMBIENT, SoundSource::Blocks,
                                         1.0f, 1.0f, false);
                }
            } else if (random.NextInt(40) == 0) {
                level.PlayLocalSound(Centre(pos), SoundEvents::DRIED_GHAST_AMBIENT_WATER, SoundSource::Blocks,
                                     1.0f, 1.0f, false);
            }
        }

        // MC SandBlock.animateTick → AmbientDesertBlockSoundsPlayer
        // .playAmbientSandSounds: open to the air, one in 2100, in a desert.
        void SandSound(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks || BlockAt(*blocks, pos + glm::ivec3(0, 1, 0)) != BlockID::Air) return;
            if (random.NextInt(2100) == 0 && ShouldPlayAmbientSandSound(*blocks, pos)) {
                level.PlayLocalSound(Corner(pos), SoundEvents::SAND_IDLE, SoundSource::Ambient, 1.0f, 1.0f, false);
            }
        }

        // MC DryVegetationBlock.animateTick → playAmbientDeadBushSounds.
        void DeadBushSound(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            if (random.NextInt(130) != 0) return;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            const glm::ivec3 below = pos - glm::ivec3(0, 1, 0);
            const BlockID belowId = BlockAt(*blocks, below);
            if ((belowId == BlockID::RedSand || Terracotta().Has(belowId)) && random.NextInt(3) != 0) return;
            if (ShouldPlayDesertDryVegetationBlockSounds(*blocks, below)) {
                level.PlayLocalSound(Corner(pos), SoundEvents::DEAD_BUSH_IDLE, SoundSource::Ambient, 1.0f, 1.0f, false);
            }
        }

        // Aurelith's river Vesper (resonant_water): never flowing, so MC's
        // flowing-water rule (FluidAnimateTickSounds) never speaks for it.
        // Only a surface cell (open air above) laps, one landing in 160 —
        // ClientLevel.doAnimateTick lands a few times a tick on the river's
        // cells round the player, so the channel murmurs every second or
        // two, and never from under the paving or down the Listening Well.
        // obeycraft:ambient.aurelith.river mixes low water with the odd
        // bubble and, rarely, a glassy note (the river "carries the Chord").
        void VesperSound(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            if (random.NextInt(160) != 0) return;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks || BlockAt(*blocks, pos + glm::ivec3(0, 1, 0)) != BlockID::Air) return;
            const glm::dvec3 at(pos.x + random.NextDouble(), pos.y + 0.9, pos.z + random.NextDouble());
            level.PlayLocalSound(at, "obeycraft:ambient.aurelith.river", SoundSource::Ambient,
                                 0.6f + random.NextFloat() * 0.4f, 0.85f + random.NextFloat() * 0.3f, false);
        }

        // ── Chaining ────────────────────────────────────────────────────────
        //
        // animateTick is a plain function pointer; a block that already has
        // one (sand's falling dust, the nether portal's motes if they are ever
        // wired) keeps it, called first, then the sound.
        std::array<BlockAnimateTickFn, BlockRegistry::Size> s_previous{};
        std::array<BlockAnimateTickFn, BlockRegistry::Size> s_sound{};

        void Chained(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            const size_t i = static_cast<size_t>(state.Block());
            if (i >= BlockRegistry::Size) return;
            if (s_previous[i]) s_previous[i](level, pos, state, random);
            if (s_sound[i]) s_sound[i](level, pos, state, random);
        }

        void Attach(std::array<Block, BlockRegistry::Size>& blocks, BlockID id, BlockAnimateTickFn sound) {
            const size_t i = static_cast<size_t>(id);
            if (i >= BlockRegistry::Size) return;
            if (blocks[i].animateTick == &Chained) {   // attached twice: keep the first
                return;
            }
            s_previous[i] = blocks[i].animateTick;
            s_sound[i] = sound;
            blocks[i].animateTick = &Chained;
        }

        bool EndsWith(const std::string& s, std::string_view suffix) {
            return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

    } // namespace

    void RegisterAmbientBlockSounds(std::array<Block, BlockRegistry::Size>& blocks) {
        Attach(blocks, BlockID::Fire, &FireSound);
        Attach(blocks, BlockID::SoulFire, &FireSound);
        Attach(blocks, BlockID::Campfire, &CampfireSound);
        Attach(blocks, BlockID::SoulCampfire, &CampfireSound);
        Attach(blocks, BlockID::Furnace, &CookerSound<&kFurnaceCrackle>);
        Attach(blocks, BlockID::Smoker, &CookerSound<&kSmokerSmoke>);
        Attach(blocks, BlockID::BlastFurnace, &CookerSound<&kBlastCrackle>);
        Attach(blocks, BlockID::RespawnAnchor, &RespawnAnchorSound);
        Attach(blocks, BlockID::BubbleColumn, &BubbleColumnSound);
        Attach(blocks, BlockID::NetherPortal, &NetherPortalSound);
        Attach(blocks, BlockID::FireflyBush, &FireflyBushSound);
        Attach(blocks, BlockID::PaleHangingMoss, &PaleHangingMossSound);
        Attach(blocks, BlockID::OpenEyeblossom, &OpenEyeblossomSound);
        Attach(blocks, BlockID::CreakingHeart, &CreakingHeartSound);
        Attach(blocks, BlockID::DriedGhast, &DriedGhastSound);
        // SandBlock covers sand and red sand; suspicious sand is a
        // BrushableBlock and plays nothing.
        Attach(blocks, BlockID::Sand, &SandSound);
        Attach(blocks, BlockID::RedSand, &SandSound);
        Attach(blocks, BlockID::DeadBush, &DeadBushSound);
        // Every candle colour and every candle cake (AbstractCandleBlock).
        for (size_t i = 0; i < BlockRegistry::Size; ++i) {
            const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
            if (slug == "candle" || EndsWith(slug, "_candle") || EndsWith(slug, "candle_cake")) {
                Attach(blocks, static_cast<BlockID>(i), &CandleSound);
            }
        }
    }

    void RegisterAurelithAmbientSounds(std::array<Block, BlockRegistry::Size>& blocks) {
        Attach(blocks, BlockID::ResonantWater, &VesperSound);
    }

    void FluidAnimateTickSounds(EntityLevel& level, const glm::ivec3& pos, FluidState fluid, JavaRandom& random) {
        if (fluid.Is(FluidType::Water)) {
            // MC WaterFluid.animateTick:47 — flowing (not source, not
            // falling) water, one in 64.
            if (!fluid.IsSource() && !fluid.falling && random.NextInt(64) == 0) {
                const float volume = random.NextFloat() * 0.25f + 0.75f;
                level.PlayLocalSound(Centre(pos), SoundEvents::WATER_AMBIENT, SoundSource::Ambient, volume,
                                     random.NextFloat() + 0.5f, false);
            }
            return;
        }
        if (fluid.Is(FluidType::Lava)) {
            // MC LavaFluid.animateTick:50 — only under open air: a pop (one in
            // 100, at a random point on the surface) and the ambient roar (one
            // in 200, at the corner).
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks || blocks->GetBlock(pos.x, pos.y + 1, pos.z) != BlockID::Air) return;
            if (random.NextInt(100) == 0) {
                const double xx = pos.x + random.NextDouble();
                const double yy = pos.y + 1.0;
                const double zz = pos.z + random.NextDouble();
                const float volume = 0.2f + random.NextFloat() * 0.2f;
                level.PlayLocalSound(glm::dvec3(xx, yy, zz), SoundEvents::LAVA_POP, SoundSource::Ambient, volume,
                                     0.9f + random.NextFloat() * 0.15f, false);
            }
            if (random.NextInt(200) == 0) {
                const float volume = 0.2f + random.NextFloat() * 0.2f;
                level.PlayLocalSound(Corner(pos), SoundEvents::LAVA_AMBIENT, SoundSource::Ambient, volume,
                                     0.9f + random.NextFloat() * 0.15f, false);
            }
        }
    }

} // namespace Game
