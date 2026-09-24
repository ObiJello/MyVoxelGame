// File: src/common/world/block/BlockGrowth.cpp
//
// Per-block growth callbacks — the third file in the same family as
// BlockBehaviors.cpp (right-click reactions) and BlockPlacement.cpp (placement
// rules). BlockRegistry::Init calls BlockRegistry_RegisterGrowth once the block
// table exists, and this file fills in the `isRandomlyTicking` / `randomTick` /
// `isValidBonemealTarget` / `performBonemeal` function pointers.
//
// Every entry is a port of a BlockBehaviour subclass from
// minecraft_code_26.1-snapshot-1/decompiled_net/minecraft/world/level/block/. The constants
// here are MC's constants — 25.0F, 1/10, 0.5714286 and friends — and they are
// what makes a wheat field take the same real time it does in vanilla. When
// something looks like an arbitrary magic number, it is quoted from the class
// it came from; check there before changing it.
//
// Two engine-wide substitutions apply throughout, both documented at their
// definitions rather than repeated at every call site:
//   • light  — IBlockAccess::GetRawBrightness is a sky-exposure stand-in
//              (there is no light engine). MC's `>= 9` / `>= 8` comparisons
//              are kept literally so the port is a one-function swap.
//   • rain   — there is no weather, so FarmBlock's `isRainingAt` branch is
//              always false. Farmland hydrates from water alone, which is what
//              vanilla does under a clear sky anyway.
#include "BlockRegistry.hpp"
#include "BlockPlacement.hpp"
#include "Direction.hpp"
#include "GeneratedBlockStates.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Game {

    namespace {

        // ── Property helpers ────────────────────────────────────────────────
        //
        // BlockStateDefinition is string-keyed (see BlockRegistry.hpp), so an
        // integer property round-trips through text. These two wrappers are the
        // only place that conversion happens, and they own the digit table so
        // no growth path ever allocates.

        constexpr std::string_view kDigits[] = {
            "0", "1", "2",  "3",  "4",  "5",  "6",  "7",
            "8", "9", "10", "11", "12", "13", "14", "15",
        };

        int IntProperty(BlockState state, std::string_view prop) {
            const std::string_view v = state.GetValueByName(prop);
            // Empty means the block doesn't declare the property. Answering 0
            // matches what the state index itself would mean (index 0 is always
            // the default) and keeps a mis-registered block inert rather than
            // crashing.
            for (int i = 0; i < static_cast<int>(std::size(kDigits)); ++i) {
                if (v == kDigits[i]) return i;
            }
            return 0;
        }

        // State index for `prop = value`, every other property left at its
        // default. MC's `state.setValue(AGE, n)` preserves the other values;
        // ours only differs for blocks that carry more than one property, and
        // the two that do (bamboo, cocoa) set their properties together below.
        BlockState StateWithInt(BlockID id, std::string_view prop, int value) {
            if (value < 0) value = 0;
            if (value >= static_cast<int>(std::size(kDigits))) {
                value = static_cast<int>(std::size(kDigits)) - 1;
            }
            return BlockStates::FromIndex(
                id, BlockRegistry::GetStateDefinition(id).IndexOfSingle(prop, kDigits[value]));
        }

        int AgeOf(BlockState state)      { return IntProperty(state, "age"); }
        int MoistureOf(BlockState state) { return IntProperty(state, "moisture"); }
        BlockState StateForAge(BlockID id, int age) { return StateWithInt(id, "age", age); }

        // ── Update flags ────────────────────────────────────────────────────
        //
        // MC's `level.setBlock(pos, state, 2)` is UPDATE_CLIENTS: tell the
        // clients, do not run neighbour updates. Growth uses it everywhere
        // because a crop advancing an age affects nothing around it. Our
        // MarkDirty is the same idea — the section is remeshed and the change
        // is broadcast (World::SetBlock always feeds the accumulator), but no
        // neighbour is notified.
        //
        // `setBlockAndUpdate` (MC flag 3) is the one that DOES notify, and MC
        // uses it exactly where a new block appears that neighbours must react
        // to: a melon spawning, a cane growing upward.
        constexpr uint32_t kUpdateClients = World::UpdateFlags::MarkDirty;
        constexpr uint32_t kUpdateAll     = World::UpdateFlags::All;

        // ── Block tags, inlined ─────────────────────────────────────────────
        // Same approach as BlockPlacement.cpp's kDirtTag: the game has no tag
        // loader, and these lists are short and stable. Matched against model
        // names, which are the MC block names.

        // data/minecraft/tags/block/dirt.json
        constexpr std::string_view kDirtTag[] = {
            "dirt", "grass_block", "grass_block_snow", "podzol", "coarse_dirt",
            "mycelium", "rooted_dirt", "moss_block", "pale_moss_block", "mud",
            "muddy_mangrove_roots",
            "sculk_loam", "hush_moss",   // The Hush soils (mirrored in dirt.json)
            // The Aether's ground: its data pack puts #aether:aether_dirt
            // (aether grass + aether dirt) into #minecraft:dirt (mirrored in
            // dirt.json), which is what lets skyroot/golden oak saplings,
            // the flowers and the berry bush stand on it.
            "aether_grass_block", "aether_dirt",
        };

        // data/minecraft/tags/block/sand.json
        constexpr std::string_view kSandTag[] = { "sand", "red_sand", "suspicious_sand" };

        // data/minecraft/tags/block/bamboo_plantable_on.json — the dirt tag
        // plus sand, plus bamboo's own two blocks (so a stalk counts as
        // plantable ground for the stalk above it).
        // data/minecraft/tags/block/maintains_farmland.json — the crops that
        // stop dry farmland reverting to dirt. Note this is NOT "every crop":
        // vanilla deliberately leaves sweet berry bushes and nether wart out,
        // because neither is planted on farmland in the first place.
        constexpr std::string_view kMaintainsFarmlandTag[] = {
            "pumpkin_stem", "attached_pumpkin_stem", "melon_stem",
            "attached_melon_stem", "beetroots", "carrots", "potatoes",
            "torchflower_crop", "torchflower", "pitcher_crop", "wheat",
        };

        bool InTag(BlockID id, const std::string_view* tag, size_t count) {
            const std::string& n = BlockRegistry::Get(id).modelName;
            for (size_t i = 0; i < count; ++i) if (n == tag[i]) return true;
            return false;
        }
        template <size_t N>
        bool InTag(BlockID id, const std::string_view (&tag)[N]) { return InTag(id, tag, N); }

        bool IsDirtTag(BlockID id) { return InTag(id, kDirtTag); }
        bool IsSandTag(BlockID id) { return InTag(id, kSandTag); }

        bool IsBambooPlantableOn(BlockID id) {
            return IsDirtTag(id) || IsSandTag(id) ||
                   id == BlockID::Bamboo || id == BlockID::BambooSapling;
        }

        // ── Small shared utilities ──────────────────────────────────────────

        BlockID BlockAt(const IBlockAccess& level, const glm::ivec3& p) {
            return level.GetBlock(p.x, p.y, p.z);
        }
        BlockState StateAt(const IBlockAccess& level, const glm::ivec3& p) {
            return level.GetBlockState(p.x, p.y, p.z);
        }
        bool IsAir(const IBlockAccess& level, const glm::ivec3& p) {
            return BlockAt(level, p) == BlockID::Air;
        }
        bool SetAt(ILevelWrite& level, const glm::ivec3& p, BlockState state, uint32_t flags) {
            return level.SetBlock(p.x, p.y, p.z, state, flags);
        }
        // "This block, in its DEFAULT state" — the sentinel dance this used to
        // need is gone: BlockStates::Default names the thing directly.
        bool SetAt(ILevelWrite& level, const glm::ivec3& p, BlockID id, uint32_t flags) {
            return SetAt(level, p, BlockStates::Default(id), flags);
        }

        // MC Direction.Plane.HORIZONTAL's face array order. A single
        // nextInt(4) indexes it — see RandomHorizontalIndex.
        constexpr Direction kHorizontalPlane[] = {
            Direction::North, Direction::East, Direction::South, Direction::West,
        };

        // MC Direction.Plane.HORIZONTAL.getRandomDirection(random) — one
        // nextInt(4) into the array above. The INDEX is returned rather than
        // the direction so callers can scan outward from the rolled choice
        // while still consuming exactly one random number, which is what keeps
        // stem growth paced the same as vanilla.
        int RandomHorizontalIndex(JavaRandom& random) {
            return random.NextInt(4);
        }

        // ────────────────────────────────────────────────────────────────────
        // CropBlock — wheat, carrots, potatoes, and the base for beetroot,
        // torchflower and (via its own class) the stems.
        // CropBlock.java
        // ────────────────────────────────────────────────────────────────────

        // Each CropBlock subclass overrides getMaxAge(); everything else about
        // the tick is shared. Rather than a function pointer per crop, the max
        // age is derived from the block's own state definition — beetroots
        // declare age 0..3, wheat 0..7 — which is the same information MC keeps
        // in getMaxAge(), just read from one place instead of two.
        int MaxAgeOf(BlockID id) {
            // The one crop where the two disagree. TorchflowerCropBlock declares
            // AGE_1 (states 0 and 1) but returns 2 from getMaxAge(), because its
            // final step is not a state at all: getStateForAge(2) returns
            // Blocks.TORCHFLOWER. Deriving the max from the property alone would
            // cap the crop at age 1 and it would never flower.
            if (id == BlockID::TorchflowerCrop) return 2;

            const auto& def = BlockRegistry::GetStateDefinition(id);
            for (const auto& p : def.properties) {
                if (p.name == "age") return static_cast<int>(p.values.size()) - 1;
            }
            return 0;
        }

        // MC CropBlock.getStateForAge + setBlock, with TorchflowerCropBlock's
        // override folded in. Every crop growth step goes through here so the
        // "grows into a different block" case is handled once.
        void ApplyCropAge(ILevelWrite& level, const glm::ivec3& pos, BlockID id, int age) {
            if (id == BlockID::TorchflowerCrop && age >= 2) {
                // The crop becomes the flower. UpdateAll rather than
                // UpdateClients: this is a new block, not an age step, and
                // anything resting on it should get a chance to react.
                SetAt(level, pos, BlockID::Torchflower, kUpdateAll);
                return;
            }
            SetAt(level, pos, StateForAge(id, age), kUpdateClients);
        }

        // CropBlock.getGrowthSpeed (CropBlock.java:95-134), ported literally.
        //
        // The shape of the result is worth understanding before touching it:
        // the 3x3 of farmland UNDER and around the crop contributes 1.0 dry or
        // 3.0 moist at the centre and a quarter of that at each of the 8
        // neighbours, so a fully moist, fully tilled plot gives 1 + 3 + 8*0.75
        // = 10.0. The second half then HALVES that if the same crop is planted
        // in a solid row or block around this one — vanilla's nudge toward
        // planting in rows with gaps.
        //
        // `type` is the crop being grown; neighbours only count if they are the
        // same crop, so wheat next to carrots does not slow either down.
        float GetGrowthSpeed(const IBlockAccess& level, const glm::ivec3& pos, BlockID type) {
            float speed = 1.0f;
            const glm::ivec3 below{pos.x, pos.y - 1, pos.z};

            for (int xx = -1; xx <= 1; ++xx) {
                for (int zz = -1; zz <= 1; ++zz) {
                    float blockSpeed = 0.0f;
                    const glm::ivec3 p{below.x + xx, below.y, below.z + zz};
                    if (BlockAt(level, p) == BlockID::Farmland) {
                        blockSpeed = 1.0f;
                        if (MoistureOf(StateAt(level, p)) > 0) {
                            blockSpeed = 3.0f;
                        }
                    }
                    if (xx != 0 || zz != 0) blockSpeed /= 4.0f;
                    speed += blockSpeed;
                }
            }

            const glm::ivec3 north{pos.x, pos.y, pos.z - 1};
            const glm::ivec3 south{pos.x, pos.y, pos.z + 1};
            const glm::ivec3 west {pos.x - 1, pos.y, pos.z};
            const glm::ivec3 east {pos.x + 1, pos.y, pos.z};

            const bool horizontal = BlockAt(level, west)  == type || BlockAt(level, east)  == type;
            const bool vertical   = BlockAt(level, north) == type || BlockAt(level, south) == type;
            if (horizontal && vertical) {
                speed /= 2.0f;
            } else {
                const bool diagonal =
                    BlockAt(level, {west.x,  west.y,  north.z}) == type ||
                    BlockAt(level, {east.x,  east.y,  north.z}) == type ||
                    BlockAt(level, {east.x,  east.y,  south.z}) == type ||
                    BlockAt(level, {west.x,  west.y,  south.z}) == type;
                if (diagonal) speed /= 2.0f;
            }
            return speed;
        }

        // The shared body of CropBlock.randomTick (CropBlock.java:73-84).
        // Split out because beetroot and torchflower wrap it in their own
        // probability gate and the stems reuse only the speed half.
        void CropRandomTickBody(ILevelWrite& level, const glm::ivec3& pos,
                                BlockState state, JavaRandom& random) {
            const BlockID id = state.Block();
            if (level.GetRawBrightness(pos.x, pos.y, pos.z) < 9) return;
            const int age = AgeOf(state);
            const int maxAge = MaxAgeOf(id);
            if (age >= maxAge) return;

            const float growthSpeed = GetGrowthSpeed(level, pos, id);
            // MC: `random.nextInt((int)(25.0F / growthSpeed) + 1) == 0`.
            // The int truncation is load-bearing — at speed 10 this is
            // nextInt(3), a 1-in-3 chance, not 1-in-3.5.
            if (random.NextInt(static_cast<int32_t>(25.0f / growthSpeed) + 1) != 0) return;

            ApplyCropAge(level, pos, id, age + 1);
        }

        // ── Grass and mycelium spread ─────────────────────────────────────
        // MC SpreadingSnowyBlock (GrassBlock and MyceliumBlock, base block
        // dirt). Two stand-ins for the missing light engine:
        //   * "light dampening into the top face < 15" — the block above
        //     lets some light through — is "the block above is not an opaque
        //     full cube", the same test the mesher culls faces with;
        //   * getMaxLocalRawBrightness(above) is the raw sky column
        //     (GetRawBrightness, which crops read) minus the time-of-day
        //     darkening, which is what keeps grass from spreading at night.

        bool SpreadingSnowLayerOfOne(BlockState above) {
            return above.Block() == BlockID::SnowLayer && above.GetValueByName("layers") == "1";
        }

        // MC FluidState.isFull: a source (or a falling column, level 8).
        bool SpreadingFluidIsFull(BlockState above) {
            if (BlockRegistry::IsWaterSource(above)) return true;
            return above.Block() == BlockID::Lava && above.GetValueByName("level") == "0";
        }

        bool SpreadingDampensAllLight(BlockState above) {
            return BlockRegistry::Get(above.Block()).opaque && BlockRegistry::IsOcclusionFullCube(above);
        }

        // SpreadingSnowyBlock.canStayAlive.
        bool SpreadingCanStayAlive(const IBlockAccess& level, const glm::ivec3& pos) {
            const BlockState above = StateAt(level, pos + glm::ivec3(0, 1, 0));
            if (SpreadingSnowLayerOfOne(above)) return true;
            if (SpreadingFluidIsFull(above)) return false;
            return !SpreadingDampensAllLight(above);
        }

        // SpreadingSnowyBlock.canPropagate: alive, and no water of any depth
        // above the target.
        bool SpreadingCanPropagate(const IBlockAccess& level, const glm::ivec3& pos) {
            return SpreadingCanStayAlive(level, pos) &&
                   !BlockRegistry::ContainsWater(StateAt(level, pos + glm::ivec3(0, 1, 0)));
        }

        // SnowyBlock.isSnowySetting: the block above is in #minecraft:snow
        // (snow layer, snow block, powder snow).
        bool SpreadingSnowySetting(BlockState above) {
            const BlockID id = above.Block();
            return id == BlockID::SnowLayer || id == BlockID::Snow || id == BlockID::PowderSnow;
        }

        // MC Level.getMaxLocalRawBrightness(pos): max(block, sky - skyDarken).
        int SpreadingMaxLocalRawBrightness(const ILevelWrite& level, const glm::ivec3& pos) {
            const World* world = dynamic_cast<const World*>(&level);
            const int darken = world ? world->GetSkyDarken() : 0;
            return level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z, darken);
        }

        bool SpreadingIsRandomlyTicking(BlockState /*state*/) { return true; }

        // The block a spreading block dies back to and spreads onto: dirt for
        // grass and mycelium (SpreadingSnowyBlock's `baseBlock`), aether dirt
        // for the Aether's grass — AetherGrassBlock.randomTick is
        // SpreadingSnowyDirtBlock.randomTick with AETHER_DIRT in both places.
        BlockID SpreadingBaseBlock(BlockID self) {
            return self == BlockID::AetherGrassBlock ? BlockID::AetherDirt : BlockID::Dirt;
        }

        // SpreadingSnowyBlock.randomTick. Dies to dirt when it cannot stay
        // alive; otherwise, in enough light, four tries at a dirt block in
        // the 3x5x3 around it, each becoming this block (snowy under snow).
        void SpreadingRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                                 BlockState state, JavaRandom& random) {
            constexpr glm::ivec3 kUp(0, 1, 0);
            const BlockID base = SpreadingBaseBlock(state.Block());
            if (!SpreadingCanStayAlive(level, pos)) {
                SetAt(level, pos, base, kUpdateAll);
                return;
            }
            if (SpreadingMaxLocalRawBrightness(level, pos + kUp) < 9) return;
            const BlockState spread = BlockStates::Default(state.Block());
            for (int i = 0; i < 4; ++i) {
                // Argument order is the RNG order: x, then y, then z.
                const int dx = random.NextInt(3) - 1;
                const int dy = random.NextInt(5) - 3;
                const int dz = random.NextInt(3) - 1;
                const glm::ivec3 target = pos + glm::ivec3(dx, dy, dz);
                if (BlockAt(level, target) != base) continue;
                if (!SpreadingCanPropagate(level, target)) continue;
                const bool snowy = SpreadingSnowySetting(StateAt(level, target + kUp));
                SetAt(level, target, spread.SetName(PropertyId::SNOWY, snowy ? "true" : "false"), kUpdateAll);
            }
        }

        bool CropIsRandomlyTicking(BlockState /*state*/) {
            // MC's `!isMaxAge(state)` needs the block id to know the max, which
            // this signature deliberately does not carry (it is called for
            // every sampled position, so it must stay a cheap filter). The tick
            // body re-checks the age and no-ops at max, so the only cost of
            // answering true here is one dispatch on an already-grown crop.
            //
            // The section-level counter that decides whether a section is
            // sampled at all is what keeps this from mattering: it counts
            // blocks, not states, so a section of mature wheat is sampled
            // either way. Returning false per-state would not save the walk.
            return true;
        }

        void CropRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                            BlockState state, JavaRandom& random) {
            CropRandomTickBody(level, pos, state, random);
        }

        // BeetrootBlock.randomTick — `if (random.nextInt(3) != 0) super(...)`,
        // i.e. beetroot only attempts growth on 2 ticks in 3.
        // TorchflowerCropBlock.randomTick is the same gate.
        void SlowCropRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                                BlockState state, JavaRandom& random) {
            if (random.NextInt(3) != 0) {
                CropRandomTickBody(level, pos, state, random);
            }
        }

        // CropBlock.growCrops + getBonemealAgeIncrease.
        bool CropIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                       BlockState state) {
            const BlockID id = state.Block();
            return AgeOf(state) < MaxAgeOf(id);            // MC: !isMaxAge(state)
        }

        void CropPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos,
                                 BlockState state, JavaRandom& random) {
            const BlockID id = state.Block();
            // MC getBonemealAgeIncrease: Mth.nextInt(random, 2, 5), inclusive.
            int increase = random.NextInt(2, 5);
            // BeetrootBlock divides that by 3 — integer division, so a roll of
            // 2 gives 0 and the bone meal is genuinely wasted. That is vanilla
            // behaviour, not a bug to round up.
            if (id == BlockID::Beetroots) increase /= 3;
            // TorchflowerCropBlock.getBonemealAgeIncrease returns a flat 1.
            if (id == BlockID::TorchflowerCrop) increase = 1;

            const int maxAge = MaxAgeOf(id);
            const int age = std::min(maxAge, AgeOf(state) + increase);
            ApplyCropAge(level, pos, id, age);
        }

        // ────────────────────────────────────────────────────────────────────
        // NetherWartBlock — NOT a CropBlock. No light gate, no growth speed,
        // no bone meal; a flat 1-in-10 per random tick.
        // NetherWartBlock.java:22-28
        // ────────────────────────────────────────────────────────────────────
        void NetherWartRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                                  BlockState state, JavaRandom& random) {
            const BlockID id = state.Block();
            const int age = AgeOf(state);
            if (age < 3 && random.NextInt(10) == 0) {
                SetAt(level, pos, StateForAge(id, age + 1), kUpdateClients);
            }
        }

        // ────────────────────────────────────────────────────────────────────
        // StemBlock — melon and pumpkin. StemBlock.java:25-70
        // ────────────────────────────────────────────────────────────────────

        struct StemPair { BlockID stem; BlockID attached; BlockID fruit; };
        constexpr StemPair kStemPairs[] = {
            { BlockID::MelonStem,   BlockID::AttachedMelonStem,   BlockID::Melon   },
            { BlockID::PumpkinStem, BlockID::AttachedPumpkinStem, BlockID::Pumpkin },
        };
        const StemPair* StemPairFor(BlockID stem) {
            for (const auto& p : kStemPairs) if (p.stem == stem) return &p;
            return nullptr;
        }

        void StemRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                            BlockState state, JavaRandom& random) {
            const BlockID id = state.Block();
            const StemPair* pair = StemPairFor(id);
            if (!pair) return;
            if (level.GetRawBrightness(pos.x, pos.y, pos.z) < 9) return;

            // Note the ordering difference from CropBlock: the stem rolls the
            // growth chance FIRST and only then looks at its age, so the roll
            // is consumed on a mature stem too. That is what paces fruit
            // spawning at the same rate as an age step.
            const float growthSpeed = GetGrowthSpeed(level, pos, id);
            if (random.NextInt(static_cast<int32_t>(25.0f / growthSpeed) + 1) != 0) return;

            const int age = AgeOf(state);
            if (age < 7) {
                SetAt(level, pos, StateForAge(id, age + 1), kUpdateClients);
                return;
            }

            // ── Mature: put a fruit in a horizontal neighbour ────────────────
            //
            // MC rolls ONE direction and gives up if it isn't suitable. We keep
            // that roll (and its single nextInt(4), so growth stays paced the
            // same) but add one deliberate divergence:
            //
            //   a fruit prefers ground that is NOT farmland.
            //
            // Vanilla is happy to drop a melon onto a tilled block, which
            // silently costs you a crop slot and is the single most annoying
            // thing about melon farming. So the four neighbours are scanned
            // starting at the rolled direction, and the first one whose ground
            // is plain dirt wins.
            //
            // The fallback is exactly vanilla: if every suitable neighbour IS
            // farmland — a fruit boxed in by tilled soil, which is the case
            // where the player has clearly built for it — the rolled direction
            // is used as-is, succeeding or failing the way MC would.
            const int rolled = RandomHorizontalIndex(random);

            auto suitable = [&](Direction d, bool& outOnFarmland) {
                const glm::ivec3 rel{pos.x + StepX(d), pos.y, pos.z + StepZ(d)};
                if (!IsAir(level, rel)) return false;
                const BlockID below = BlockAt(level, {rel.x, rel.y - 1, rel.z});
                outOnFarmland = (below == BlockID::Farmland);
                return outOnFarmland || IsDirtTag(below);
            };

            Direction dir = kHorizontalPlane[rolled];
            bool haveChoice = false;
            for (int i = 0; i < 4; ++i) {
                const Direction d = kHorizontalPlane[(rolled + i) & 3];
                bool onFarmland = false;
                if (!suitable(d, onFarmland)) continue;
                if (!onFarmland) { dir = d; haveChoice = true; break; }
            }
            if (!haveChoice) {
                // Every candidate was farmland (or there were none) — vanilla
                // behaviour on the direction actually rolled.
                bool onFarmland = false;
                if (!suitable(dir, onFarmland)) return;
            }

            const glm::ivec3 relative{pos.x + StepX(dir), pos.y, pos.z + StepZ(dir)};
            SetAt(level, relative, pair->fruit, kUpdateAll);
            // The stem turns into its attached form pointing AT the fruit.
            const BlockState facingState = BlockStates::FromIndex(
                pair->attached,
                BlockRegistry::GetStateDefinition(pair->attached)
                    .IndexOfSingle("facing", NameOf(dir)));
            SetAt(level, pos, facingState, kUpdateAll);
        }

        // AttachedStemBlock.updateShape (AttachedStemBlock.java):
        //
        //     if (!neighbourState.is(this.fruit) && directionToNeighbour == state.getValue(FACING))
        //         return stem.defaultBlockState().trySetValue(StemBlock.AGE, 7);
        //
        // Pick the melon and the stem it grew from turns back into an ordinary
        // stem at max age — which is immediately eligible to grow another
        // fruit on its next random tick. Without it a harvested stem is a dead
        // end and the patch produces exactly one melon, ever.
        //
        // The FACING check is what keeps it from firing on any other
        // neighbour: only a change in the direction the stem points at means
        // the fruit is gone.
        bool AttachedStemUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockState state,
                                         Direction toNeighbour, BlockID neighbourId,
                                         BlockState& outState,
                                         ScheduledTickAccess* /*ticks*/) {
            (void)level; (void)pos;
            const BlockID id = state.Block();
            const StemPair* pair = nullptr;
            for (const auto& p : kStemPairs) {
                if (p.attached == id) { pair = &p; break; }
            }
            if (!pair) return false;

            const std::string_view facing = state.GetValueByName("facing");
            if (facing != NameOf(toNeighbour)) return false;   // not the fruit's side
            if (neighbourId == pair->fruit) return false;      // fruit still there

            // One value now: the block is part of the state.
            outState = StateForAge(pair->stem, 7);
            return true;
        }

        bool StemIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                       BlockState state) {
            return AgeOf(state) != 7;   // MC: AGE != 7
        }

        void StemPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos,
                                 BlockState state, JavaRandom& random) {
            const BlockID id = state.Block();
            const int age = std::min(7, AgeOf(state) + random.NextInt(2, 5));
            const BlockState newState = StateForAge(id, age);
            SetAt(level, pos, newState, kUpdateClients);
            // MC: `if (age == 7) newState.randomTick(level, pos, random);` —
            // bone-mealing a stem to maturity gives it an immediate shot at
            // spawning its fruit rather than making you wait for a random tick.
            if (age == 7) StemRandomTick(level, pos, newState, random);
        }

        // ────────────────────────────────────────────────────────────────────
        // SugarCaneBlock — SugarCaneBlock.java:21-36
        // ────────────────────────────────────────────────────────────────────
        void SugarCaneRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                                 BlockState state, JavaRandom& random) {
            const BlockID id = state.Block();
            (void)random;   // cane growth is deterministic once the tick lands
            const glm::ivec3 above{pos.x, pos.y + 1, pos.z};
            if (!IsAir(level, above)) return;

            // Count how tall this stack already is. MC's loop starts at 1 and
            // walks DOWN, so `height` ends up as the number of cane blocks at
            // and below this one — the cap is on total stack height, not on
            // distance from the ground.
            int height = 1;
            while (level.GetBlock(pos.x, pos.y - height, pos.z) == id) ++height;
            if (height >= 3) return;

            const int age = AgeOf(state);
            if (age == 15) {
                SetAt(level, above, id, kUpdateAll);
                SetAt(level, pos, StateForAge(id, 0), kUpdateClients);
            } else {
                SetAt(level, pos, StateForAge(id, age + 1), kUpdateClients);
            }
        }

        // ── Sugar cane bone meal — BEDROCK behaviour, not Java ──────────────
        //
        // Java Edition's sugar cane is not bonemealable at all. Bedrock's is:
        // bone meal grows the stalk by one to two blocks instantly, the same
        // shape as bamboo's. Requested deliberately, so this is a knowing
        // divergence from the decompile rather than an oversight.
        //
        // The three-block cap and the "needs clear air above" rule are still
        // the random-tick ones, so bone meal can only do what waiting would
        // eventually do — it just skips the wait.
        constexpr int kSugarCaneMaxHeight = 3;

        int SugarCaneHeightBelow(const IBlockAccess& level, const glm::ivec3& pos) {
            int n = 0;
            while (n < kSugarCaneMaxHeight &&
                   level.GetBlock(pos.x, pos.y - (n + 1), pos.z) == BlockID::SugarCane) {
                ++n;
            }
            return n;
        }
        int SugarCaneHeightAbove(const IBlockAccess& level, const glm::ivec3& pos) {
            int n = 0;
            while (n < kSugarCaneMaxHeight &&
                   level.GetBlock(pos.x, pos.y + (n + 1), pos.z) == BlockID::SugarCane) {
                ++n;
            }
            return n;
        }

        bool SugarCaneIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                            BlockState /*state*/) {
            // Measured over the WHOLE stalk, so bone-mealing the bottom segment
            // of a full-height cane correctly does nothing instead of silently
            // consuming the item.
            const int above = SugarCaneHeightAbove(level, pos);
            const int total = above + SugarCaneHeightBelow(level, pos) + 1;
            if (total >= kSugarCaneMaxHeight) return false;
            const glm::ivec3 top{pos.x, pos.y + above, pos.z};
            return IsAir(level, {top.x, top.y + 1, top.z});
        }

        void SugarCanePerformBonemeal(ILevelWrite& level, const glm::ivec3& pos,
                                      BlockState state, JavaRandom& random) {
            const BlockID id = state.Block();
            int above = SugarCaneHeightAbove(level, pos);
            int total = above + SugarCaneHeightBelow(level, pos) + 1;
            const int toGrow = 1 + random.NextInt(2);   // Bedrock: one or two

            for (int i = 0; i < toGrow; ++i) {
                if (total >= kSugarCaneMaxHeight) return;
                const glm::ivec3 top{pos.x, pos.y + above, pos.z};
                const glm::ivec3 newPos{top.x, top.y + 1, top.z};
                if (!IsAir(level, newPos)) return;
                SetAt(level, newPos, id, kUpdateAll);
                ++above;
                ++total;
            }
        }

        // ────────────────────────────────────────────────────────────────────
        // CactusBlock — CactusBlock.java:24-52
        // ────────────────────────────────────────────────────────────────────
        void CactusRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                              BlockState state, JavaRandom& random) {
            const BlockID id = state.Block();
            const glm::ivec3 above{pos.x, pos.y + 1, pos.z};
            if (!IsAir(level, above)) return;

            const int age = AgeOf(state);
            int height = 1;
            while (level.GetBlock(pos.x, pos.y - height, pos.z) == id) {
                ++height;
                // MC bails outright for a full-height cactus that is already
                // at max age, so the top block never re-rolls its flower.
                if (height == 3 && age == 15) return;
            }

            // MC also requires `canSurvive(defaultBlockState(), level,
            // pos.above())` here — the cell the FLOWER would occupy has to be a
            // legal cactus position, which is what stops a cactus flowering
            // into a gap it is walled in against.
            if (age == 8 && CanSurviveAt(level, above, id)) {
                // ATTEMPT_GROW_CACTUS_FLOWER_AGE. A taller cactus is likelier
                // to flower: 0.25 at height >= 3, 0.1 below.
                const double chance = (height >= 3) ? 0.25 : 0.1;
                if (random.NextDouble() <= chance) {
                    SetAt(level, above, BlockID::CactusFlower, kUpdateAll);
                }
            } else if (age == 15 && height < 3) {
                SetAt(level, above, id, kUpdateAll);
                SetAt(level, pos, StateForAge(id, 0), kUpdateClients);
            }

            if (age < 15) {
                SetAt(level, pos, StateForAge(id, age + 1), kUpdateClients);
            }
        }

        // ────────────────────────────────────────────────────────────────────
        // BambooStalkBlock — BambooStalkBlock.java:60-140
        //
        // Bamboo's three properties do different jobs and are easy to confuse:
        //   age    — thin (0) vs thick (1) stalk, a LOOK, not a growth counter
        //   leaves — none / small / large, decided as the stalk extends
        //   stage  — 0 while it can still grow, 1 once it has stopped
        // ────────────────────────────────────────────────────────────────────

        constexpr int kBambooMaxHeight = 16;

        BlockState BambooState(int age, std::string_view leaves, int stage) {
            BlockRegistry::BlockStateDefinition::PropertyMap props;
            props["age"]    = std::string(kDigits[age]);
            props["leaves"] = std::string(leaves);
            props["stage"]  = std::string(kDigits[stage]);
            return BlockStates::FromIndex(
                BlockID::Bamboo,
                BlockRegistry::GetStateDefinition(BlockID::Bamboo).IndexOf(props));
        }

        std::string_view BambooLeavesOf(const IBlockAccess& level, const glm::ivec3& p) {
            if (BlockAt(level, p) != BlockID::Bamboo) return "none";
            return StateAt(level, p).GetValueByName("leaves");
        }

        int BambooHeightBelow(const IBlockAccess& level, const glm::ivec3& pos) {
            int height = 0;
            while (height < kBambooMaxHeight &&
                   level.GetBlock(pos.x, pos.y - (height + 1), pos.z) == BlockID::Bamboo) {
                ++height;
            }
            return height;
        }
        int BambooHeightAbove(const IBlockAccess& level, const glm::ivec3& pos) {
            int height = 0;
            while (height < kBambooMaxHeight &&
                   level.GetBlock(pos.x, pos.y + (height + 1), pos.z) == BlockID::Bamboo) {
                ++height;
            }
            return height;
        }

        // BambooStalkBlock.growBamboo — places ONE new stalk above `pos` and
        // may retexture the two below it. The leaf shuffling is what gives a
        // grown stalk its taper: the new top gets the large leaves and the
        // section two down loses its leaves entirely.
        void GrowBamboo(ILevelWrite& level, const glm::ivec3& pos, JavaRandom& random,
                        int height) {
            const glm::ivec3 belowPos{pos.x, pos.y - 1, pos.z};
            const glm::ivec3 twoBelowPos{pos.x, pos.y - 2, pos.z};
            const BlockID below = BlockAt(level, belowPos);
            const BlockID twoBelow = BlockAt(level, twoBelowPos);

            std::string_view leaves = "none";
            if (height >= 1) {
                if (below == BlockID::Bamboo && BambooLeavesOf(level, belowPos) != "none") {
                    leaves = "large";
                    if (twoBelow == BlockID::Bamboo) {
                        SetAt(level, belowPos,
                              BambooState(AgeOf(StateAt(level, belowPos)), "small",
                                          IntProperty(StateAt(level, belowPos), "stage")),
                              kUpdateAll);
                        SetAt(level, twoBelowPos,
                              BambooState(AgeOf(StateAt(level, twoBelowPos)), "none",
                                          IntProperty(StateAt(level, twoBelowPos), "stage")),
                              kUpdateAll);
                    }
                } else {
                    leaves = "small";
                }
            }

            const BlockState selfState = StateAt(level, pos);
            // Thick bamboo once this is the second segment or higher.
            const int age = (AgeOf(selfState) != 1 &&
                             twoBelow != BlockID::Bamboo) ? 0 : 1;
            // Stop growing near the height cap, or with 1-in-4 odds past 11.
            const int stage = ((height < 11 || !(random.NextFloat() < 0.25f)) && height != 15)
                                  ? 0 : 1;

            SetAt(level, {pos.x, pos.y + 1, pos.z},
                  BambooState(age, leaves, stage), kUpdateAll);
        }

        bool BambooIsRandomlyTicking(BlockState state) {
            // MC: STAGE == 0 — a finished stalk stops ticking for good.
            return IntProperty(state, "stage") == 0;
        }

        void BambooRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                              BlockState state, JavaRandom& random) {
            if (IntProperty(state, "stage") != 0) return;
            const glm::ivec3 above{pos.x, pos.y + 1, pos.z};
            if (random.NextInt(3) != 0) return;
            if (!IsAir(level, above)) return;
            if (level.GetRawBrightness(above.x, above.y, above.z) < 9) return;

            const int height = BambooHeightBelow(level, pos) + 1;
            if (height < kBambooMaxHeight) GrowBamboo(level, pos, random, height);
        }

        // BambooSaplingBlock.growBamboo — puts the first real stalk ABOVE the
        // shoot (the shoot itself stays put and becomes the base of the plant),
        // with SMALL leaves. Note the leaves value: a stalk grown this way is
        // NOT leafless, unlike every segment BambooStalkBlock adds later.
        void GrowBambooSapling(ILevelWrite& level, const glm::ivec3& pos) {
            SetAt(level, {pos.x, pos.y + 1, pos.z},
                  BambooState(0, "small", 0), kUpdateAll);
        }

        // BambooSaplingBlock.randomTick — a shoot becomes a stalk.
        void BambooSaplingRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                                     BlockState /*state*/,
                                     JavaRandom& random) {
            const glm::ivec3 above{pos.x, pos.y + 1, pos.z};
            if (random.NextInt(3) != 0) return;
            if (!IsAir(level, above)) return;
            if (level.GetRawBrightness(above.x, above.y, above.z) < 9) return;
            GrowBambooSapling(level, pos);
        }

        // BambooSaplingBlock's own bonemeal pair — deliberately NOT the
        // stalk's. A shoot only needs the cell above to be free; the stalk's
        // version walks the whole plant looking for a growable top, which a
        // lone shoot does not have.
        bool BambooSaplingIsValidBonemealTarget(const IBlockAccess& level,
                                                const glm::ivec3& pos,
                                                BlockState /*state*/) {
            return IsAir(level, {pos.x, pos.y + 1, pos.z});
        }

        void BambooSaplingPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos,
                                          BlockState /*state*/,
                                          JavaRandom& /*random*/) {
            GrowBambooSapling(level, pos);
        }

        bool BambooIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockState /*state*/) {
            const int above = BambooHeightAbove(level, pos);
            const int below = BambooHeightBelow(level, pos);
            if (above + below + 1 >= kBambooMaxHeight) return false;
            const glm::ivec3 top{pos.x, pos.y + above, pos.z};
            return IntProperty(StateAt(level, top), "stage") != 1;
        }

        void BambooPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos,
                                   BlockState /*state*/,
                                   JavaRandom& random) {
            int above = BambooHeightAbove(level, pos);
            const int below = BambooHeightBelow(level, pos);
            int total = above + below + 1;
            const int newBamboo = 1 + random.NextInt(2);   // 1 or 2 segments

            for (int i = 0; i < newBamboo; ++i) {
                const glm::ivec3 top{pos.x, pos.y + above, pos.z};
                if (total >= kBambooMaxHeight) return;
                if (IntProperty(StateAt(level, top), "stage") == 1) return;
                if (!IsAir(level, {top.x, top.y + 1, top.z})) return;
                GrowBamboo(level, top, random, total);
                ++above;
                ++total;
            }
        }

        // ────────────────────────────────────────────────────────────────────
        // CocoaBlock — CocoaBlock.java:13-55
        // ────────────────────────────────────────────────────────────────────
        BlockState CocoaState(const IBlockAccess& level, const glm::ivec3& pos, int age) {
            const auto& def = BlockRegistry::GetStateDefinition(BlockID::Cocoa);
            BlockRegistry::BlockStateDefinition::PropertyMap props;
            props["facing"] = std::string(StateAt(level, pos).GetValueByName("facing"));
            props["age"]    = std::string(kDigits[age]);
            return BlockStates::FromIndex(BlockID::Cocoa, def.IndexOf(props));
        }

        bool CocoaIsRandomlyTicking(BlockState state) {
            return IntProperty(state, "age") < 2;
        }

        void CocoaRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                             BlockState state, JavaRandom& random) {
            if (random.NextInt(5) != 0) return;
            const int age = AgeOf(state);
            if (age < 2) {
                SetAt(level, pos, CocoaState(level, pos, age + 1), kUpdateClients);
            }
        }

        bool CocoaIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                        BlockState state) {
            (void)level; (void)pos;
            return IntProperty(state, "age") < 2;
        }

        void CocoaPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos,
                                  BlockState state, JavaRandom& /*random*/) {
            SetAt(level, pos, CocoaState(level, pos, AgeOf(state) + 1), kUpdateClients);
        }

        // ────────────────────────────────────────────────────────────────────
        // SweetBerryBushBlock — SweetBerryBushBlock.java:27-88
        // Note it reads the brightness of the block ABOVE, not its own.
        // ────────────────────────────────────────────────────────────────────
        bool BerryIsRandomlyTicking(BlockState state) {
            return IntProperty(state, "age") < 3;
        }

        void BerryRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                             BlockState state, JavaRandom& random) {
            const BlockID id = state.Block();
            const int age = AgeOf(state);
            if (age < 3 && random.NextInt(5) == 0 &&
                level.GetRawBrightness(pos.x, pos.y + 1, pos.z) >= 9) {
                SetAt(level, pos, StateForAge(id, age + 1), kUpdateClients);
            }
        }

        bool BerryIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                        BlockState state) {
            (void)level; (void)pos;
            return IntProperty(state, "age") < 3;
        }

        void BerryPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos,
                                  BlockState state, JavaRandom& /*random*/) {
            const BlockID id = state.Block();
            SetAt(level, pos, StateForAge(id, std::min(3, AgeOf(state) + 1)), kUpdateClients);
        }

        // ────────────────────────────────────────────────────────────────────
        // FarmBlock — FarmBlock.java:76-118
        // ────────────────────────────────────────────────────────────────────

        // MC isNearWater: a 9 x 2 x 9 box from (-4, 0, -4) to (+4, +1, +4).
        // The +1 on Y is why a water block level with the farmland's TOP still
        // counts — the usual "water one block up, four out" farm layout.
        bool IsNearWater(const IBlockAccess& level, const glm::ivec3& pos) {
            for (int y = 0; y <= 1; ++y) {
                for (int x = -4; x <= 4; ++x) {
                    for (int z = -4; z <= 4; ++z) {
                        if (level.GetBlock(pos.x + x, pos.y + y, pos.z + z) == BlockID::Water) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        bool ShouldMaintainFarmland(const IBlockAccess& level, const glm::ivec3& pos) {
            return InTag(level.GetBlock(pos.x, pos.y + 1, pos.z), kMaintainsFarmlandTag);
        }

        void FarmlandRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                                BlockState state, JavaRandom& /*random*/) {
            const BlockID id = state.Block();
            const int moisture = MoistureOf(state);
            // MC also ORs in `level.isRainingAt(pos.above())`. No weather here,
            // so that term is permanently false — see the file header.
            if (!IsNearWater(level, pos)) {
                if (moisture > 0) {
                    SetAt(level, pos, StateWithInt(id, "moisture", moisture - 1), kUpdateClients);
                } else if (!ShouldMaintainFarmland(level, pos)) {
                    // MC turnToDirt. `pushEntitiesUp` is skipped: farmland and
                    // dirt have the same collision box height for our purposes
                    // (the engine's shapes are single AABBs), so nothing can be
                    // trapped by the swap.
                    SetAt(level, pos, BlockID::Dirt, kUpdateAll);
                }
            } else if (moisture < 7) {
                // Straight to 7, not +1 — hydration is instant in vanilla and
                // only drying is gradual.
                SetAt(level, pos, StateWithInt(id, "moisture", 7), kUpdateClients);
            }
        }

        bool FarmlandIsRandomlyTicking(BlockState /*state*/) { return true; }

        // ────────────────────────────────────────────────────────────────────
        // PitcherCropBlock — PitcherCropBlock.java
        //
        // Only the LOWER half ticks, and past age 3 the plant occupies two
        // blocks. The double-block half of that (placing and breaking the upper
        // block in step with the lower) needs DoublePlantBlock behaviour the
        // engine does not have yet, so what is wired here is the age counter
        // and the growth pacing; the upper half is written alongside it.
        // ────────────────────────────────────────────────────────────────────
        constexpr int kPitcherDoubleAge = 3;   // DOUBLE_PLANT_AGE_INTERSECTION

        BlockState PitcherState(int age, std::string_view half) {
            BlockRegistry::BlockStateDefinition::PropertyMap props;
            props["age"]  = std::string(kDigits[age]);
            props["half"] = std::string(half);
            return BlockStates::FromIndex(
                BlockID::PitcherCrop,
                BlockRegistry::GetStateDefinition(BlockID::PitcherCrop).IndexOf(props));
        }

        bool PitcherIsRandomlyTicking(BlockState state) {
            return state.GetValueByName("half") == "lower" &&
                   IntProperty(state, "age") < 4;
        }

        void PitcherGrow(ILevelWrite& level, const glm::ivec3& pos, int newAge) {
            SetAt(level, pos, PitcherState(newAge, "lower"), kUpdateClients);
            const glm::ivec3 above{pos.x, pos.y + 1, pos.z};
            if (newAge >= kPitcherDoubleAge) {
                // Only claim the cell above if it is free; MC's grow() checks
                // canGrow first, which comes to the same thing for our purposes.
                if (IsAir(level, above) || BlockAt(level, above) == BlockID::PitcherCrop) {
                    SetAt(level, above, PitcherState(newAge, "upper"), kUpdateAll);
                }
            }
        }

        void PitcherRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                               BlockState state, JavaRandom& random) {
            if (state.GetValueByName("half") != "lower") return;
            const int age = AgeOf(state);
            if (age >= 4) return;
            // No light gate here — PitcherCropBlock.randomTick genuinely has
            // none; light only appears in its canSurvive.
            const float growthSpeed = GetGrowthSpeed(level, pos, state.Block());
            if (random.NextInt(static_cast<int32_t>(25.0f / growthSpeed) + 1) != 0) return;
            PitcherGrow(level, pos, age + 1);
        }

        bool PitcherIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                          BlockState state) {
            (void)level; (void)pos;
            return IntProperty(state, "age") < 4;
        }

        void PitcherPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos,
                                    BlockState state, JavaRandom& /*random*/) {
            // BONEMEAL_INCREASE = 1.
            const int age = std::min(4, AgeOf(state) + 1);
            // Bone-mealing the upper half grows the plant from its lower half.
            glm::ivec3 base = pos;
            if (state.GetValueByName("half") == "upper") base.y -= 1;
            PitcherGrow(level, base, age);
        }


        // ────────────────────────────────────────────────────────────────────
        // SaplingBlock — The Hush's whisperwood sapling
        // SaplingBlock.java, and for the tree it grows: TreeGrower.growTree,
        // TreeFeature.java (place / doPlace / getMaxFreeTreeHeight /
        // updateLeaves), StraightTrunkPlacer, BlobFoliagePlacer and
        // TwoLayersFeatureSize — the same recipe the terrain library's
        // HushFeatures.cpp WHISPERWOOD is built from:
        //   createStraightBlobTree(whisperwood_log, lantern_leaves, 5, 2, 1, 2)
        //   = StraightTrunkPlacer(5, 2, 1), BlobFoliagePlacer(2, 0, 3),
        //     TwoLayersFeatureSize(1, 0, 1), ignoreVines.
        //
        // This engine has no TreeGrower, so the vanilla saplings stay inert
        // (they always have); only the whisperwood is wired, and the tree is
        // placed here directly against ILevelWrite.
        //
        // The Twilight Forest and Aether saplings (pass one) reuse the same
        // placer, parameterised per species by SaplingTreeSpec below: a
        // straight trunk + blob canopy with TwoLayersFeatureSize(1, 0, 1).
        // Two of them ARE that recipe in their mods (twilight_oak_tree.json,
        // skyroot_tree.json: StraightTrunkPlacer(4, 2, 0) + BlobFoliagePlacer
        // (2, 0, 3)); the rest are pass-one stand-ins for trees built from
        // mod-specific placers — see the table.
        // ────────────────────────────────────────────────────────────────────

        // One sapling's tree. `baseHeight + nextInt(randA + 1) + nextInt(randB
        // + 1)` is TrunkPlacer.getTreeHeight; `leafRadius` is the
        // BlobFoliagePlacer radius, `foliageHeight` its height.
        // `needsLight` is SaplingBlock.randomTick's
        // getMaxLocalRawBrightness(above) >= 9 gate.
        struct SaplingTreeSpec {
            BlockID log;
            BlockID leaves;
            int     baseHeight;
            int     heightRandA;
            int     heightRandB;
            int     leafRadius;
            int     foliageHeight;
            bool    needsLight;
        };

        const SaplingTreeSpec* SaplingTreeSpecFor(BlockID sapling) {
            // The Hush: HushFeatures.cpp WHISPERWOOD — createStraightBlobTree(
            // whisperwood_log, lantern_leaves, 5, 2, 1, 2). No light gate:
            // see WhisperwoodSaplingRandomTick's note.
            static constexpr SaplingTreeSpec kWhisperwood{
                BlockID::WhisperwoodLog, BlockID::LanternLeaves, 5, 2, 1, 2, 3, false };
            // TF twilight_oak_tree.json: StraightTrunkPlacer(4, 2, 0),
            // BlobFoliagePlacer(2, 0, 3) — exact. (TFTreeGrowers.TWILIGHT_OAK)
            static constexpr SaplingTreeSpec kTwilightOak{
                BlockID::TwilightOakLog, BlockID::TwilightOakLeaves, 4, 2, 0, 2, 3, true };
            // TF canopy_tree.json is a BranchingTrunkPlacer(20…) with
            // SpheroidFoliagePlacer (r ~4.1) — pass-one stand-in: a tall
            // straight trunk 8..11 under a radius-3 blob. Pass two ports the
            // branching canopy tree.
            static constexpr SaplingTreeSpec kCanopy{
                BlockID::CanopyLog, BlockID::CanopyLeaves, 8, 3, 0, 3, 3, true };
            // TF mangrove_tree.json (TrunkRiser + BranchingTrunkPlacer +
            // mangrove roots) and darkwood_tree.json (BranchingTrunkPlacer(9),
            // spheroid r 4.5) — pass-one stand-ins: straight 4..6, blob r2.
            static constexpr SaplingTreeSpec kTfMangrove{
                BlockID::TfMangroveLog, BlockID::TfMangroveLeaves, 4, 2, 0, 2, 3, true };
            static constexpr SaplingTreeSpec kDarkwood{
                BlockID::DarkLog, BlockID::DarkLeaves, 4, 2, 0, 2, 3, true };
            // Aether skyroot_tree.json: StraightTrunkPlacer(4, 2, 0),
            // BlobFoliagePlacer(2, 0, 3) — exact. (AetherTreeGrowers.SKYROOT)
            static constexpr SaplingTreeSpec kSkyroot{
                BlockID::SkyrootLog, BlockID::SkyrootLeaves, 4, 2, 0, 2, 3, true };
            // Aether golden_oak_tree.json is GoldenOakTrunkPlacer(10) +
            // GoldenOakFoliagePlacer (r 3) — pass-one stand-in: straight 5..7,
            // blob r3.
            static constexpr SaplingTreeSpec kGoldenOak{
                BlockID::GoldenOakLog, BlockID::GoldenOakLeaves, 5, 2, 0, 3, 3, true };
            switch (sapling) {
                case BlockID::WhisperwoodSapling:  return &kWhisperwood;
                case BlockID::TwilightOakSapling:  return &kTwilightOak;
                case BlockID::CanopySapling:       return &kCanopy;
                case BlockID::TfMangroveSapling:   return &kTfMangrove;
                case BlockID::DarkwoodSapling:     return &kDarkwood;
                case BlockID::SkyrootSapling:      return &kSkyroot;
                case BlockID::GoldenOakSapling:    return &kGoldenOak;
                default:                           return nullptr;
            }
        }

        int StageOf(BlockState state) { return IntProperty(state, "stage"); }

        bool NameEndsWith(const std::string& n, std::string_view suffix) {
            return n.size() >= suffix.size() &&
                   n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        // data/minecraft/tags/block/small_flowers.json (+ the Hush's bloom,
        // registered on the same BushBlock class with replaceableByTrees).
        constexpr std::string_view kSmallFlowersTag[] = {
            "dandelion", "open_eyeblossom", "poppy", "blue_orchid", "allium",
            "azure_bluet", "red_tulip", "orange_tulip", "white_tulip", "pink_tulip",
            "oxeye_daisy", "cornflower", "lily_of_the_valley", "wither_rose",
            "torchflower", "closed_eyeblossom", "golden_dandelion",
            "resonance_bloom",
            "white_flower", "purple_flower",   // The Aether (its small_flowers.json entries)
        };

        // TreeFeature.validTreePos: air or #minecraft:replaceable_by_trees.
        // The tag is #leaves + #small_flowers + the replaceable plants (short
        // grass, ferns, dead bush, vines, glow lichen, the tall flowers, roots,
        // leaf litter, dry grass, bush…) + water + seagrass + pale moss carpet
        // + pitcher plant + shelf mushroom. The engine's `replaceable` flag IS
        // the replaceable-plants half plus water, but it also carries lava,
        // fire, light and structure_void, none of which the tag lists — those
        // are refused explicitly.
        bool WhisperwoodValidTreePos(const IBlockAccess& level, const glm::ivec3& p) {
            const BlockID id = BlockAt(level, p);
            if (id == BlockID::Air) return true;
            if (id == BlockID::Lava || id == BlockID::Fire || id == BlockID::SoulFire ||
                id == BlockID::Light || id == BlockID::StructureVoid) {
                return false;
            }
            const Block& b = BlockRegistry::Get(id);
            if (b.replaceable) return true;
            if (NameEndsWith(b.modelName, "_leaves")) return true;
            if (IsSaplingBlock(id)) return true;
            if (b.modelName == "pale_moss_carpet" || b.modelName == "pitcher_plant" ||
                b.modelName == "shelf_mushroom") {
                return true;
            }
            for (std::string_view f : kSmallFlowersTag) if (b.modelName == f) return true;
            return false;
        }

        // TrunkPlacer.isFree: validTreePos || #minecraft:logs. The logs tag is
        // every *_log / *_wood (stripped included) plus the nether stems and
        // hyphae and bamboo_block.
        bool WhisperwoodIsFree(const IBlockAccess& level, const glm::ivec3& p) {
            if (WhisperwoodValidTreePos(level, p)) return true;
            const std::string& n = BlockRegistry::Get(BlockAt(level, p)).modelName;
            return NameEndsWith(n, "_log") || NameEndsWith(n, "_wood") ||
                   n == "crimson_stem" || n == "warped_stem" ||
                   n == "stripped_crimson_stem" || n == "stripped_warped_stem" ||
                   n == "crimson_hyphae" || n == "warped_hyphae" ||
                   n == "stripped_crimson_hyphae" || n == "stripped_warped_hyphae" ||
                   n == "bamboo_block" || n == "stripped_bamboo_block";
        }

        // TreeFeature places every trunk and foliage block with flag 19 =
        // UPDATE_NEIGHBORS | UPDATE_CLIENTS | UPDATE_KNOWN_SHAPE and then runs
        // StructureTemplate.updateShapeAtEdge over the bounds. The engine's
        // per-write updateShape walk does the edge pass for free, so the
        // KNOWN_SHAPE half is left off and each write is a plain UpdateAll.
        constexpr uint32_t kTreeFlags = World::UpdateFlags::All;

        // SaplingBlock.advanceTree / TreeGrower.growTree write the sapling
        // itself with flag 4 (UPDATE_INVISIBLE) — MC flag 260 for the stage
        // step also carries UPDATE_SKIP_BLOCK_ENTITY_SIDEEFFECTS. Neither
        // notifies clients: the stage is invisible (both stages share the
        // model) and the sapling's removal is covered by the log that
        // replaces it, or reverted before anyone could see it.
        constexpr uint32_t kSaplingInvisible = World::UpdateFlags::Invisible;
        constexpr uint32_t kSaplingStageFlags =
            World::UpdateFlags::Invisible | World::UpdateFlags::SkipBlockEntitySideEffects;

        // TwoLayersFeatureSize(limit = 1, lowerSize = 0, upperSize = 1).
        // getSizeAtHeight: y < limit ? lowerSize : upperSize.
        int WhisperwoodSizeAtHeight(int y) { return y < 1 ? 0 : 1; }

        // TreeFeature.getMaxFreeTreeHeight, with ignoreVines (no vine test).
        int WhisperwoodMaxFreeTreeHeight(const IBlockAccess& level, int maxTreeHeight,
                                         const glm::ivec3& origin) {
            for (int y = 0; y <= maxTreeHeight + 1; ++y) {
                const int r = WhisperwoodSizeAtHeight(y);
                for (int x = -r; x <= r; ++x) {
                    for (int z = -r; z <= r; ++z) {
                        if (!WhisperwoodIsFree(level, origin + glm::ivec3(x, y, z))) {
                            return y - 2;
                        }
                    }
                }
            }
            return maxTreeHeight;
        }

        // TrunkPlacer.placeBelowTrunkBlock with the default provider —
        // TreeConfiguration's dirt rule: the block under the trunk becomes
        // dirt unless it is already in #minecraft:dirt. Sculk loam and hush
        // moss are in the tag (kDirtTag), so a Hush tree keeps its soil;
        // a sapling planted on farmland turns it to dirt, as in vanilla.
        void WhisperwoodSetDirtBelow(ILevelWrite& level, const glm::ivec3& below) {
            if (IsDirtTag(BlockAt(level, below))) return;
            SetAt(level, below, BlockID::Dirt, kTreeFlags);
        }

        // FoliagePlacer.tryPlaceLeaf: never over a persistent leaf (a
        // player-placed one), only into a valid tree position; waterlogged
        // when placed into a water source.
        bool WhisperwoodTryPlaceLeaf(ILevelWrite& level, const glm::ivec3& p,
                                     BlockID leavesBlock, std::vector<glm::ivec3>& leaves) {
            const BlockState existing = StateAt(level, p);
            if (existing.HasProperty(PropertyId::PERSISTENT) &&
                existing.GetValueByName("persistent") == "true") {
                return false;
            }
            if (!WhisperwoodValidTreePos(level, p)) return false;
            // Default leaves: distance 7, persistent false — the distances
            // are filled in by WhisperwoodUpdateLeaves afterwards.
            BlockState leaf = BlockStates::Default(leavesBlock);
            if (BlockRegistry::IsWaterSource(existing)) {
                leaf = leaf.SetName(PropertyId::WATERLOGGED, "true");
            }
            SetAt(level, p, leaf, kTreeFlags);
            leaves.push_back(p);
            return true;
        }

        // FoliagePlacer.placeLeavesRow for a single trunk, with
        // BlobFoliagePlacer.shouldSkipLocation:
        //   dx == r && dz == r && (random.nextInt(2) == 0 || y == 0)
        // on the ABSOLUTE offsets (shouldSkipLocationSigned) — i.e. the four
        // corners of the square, each dropped by a coin flip, and always
        // dropped on the top row. The nextInt is only consumed at a corner,
        // which keeps the random stream in step with vanilla.
        void WhisperwoodPlaceLeavesRow(ILevelWrite& level, JavaRandom& random,
                                       const glm::ivec3& origin, int radius, int y,
                                       BlockID leavesBlock, std::vector<glm::ivec3>& leaves) {
            for (int dx = -radius; dx <= radius; ++dx) {
                for (int dz = -radius; dz <= radius; ++dz) {
                    const bool corner = std::abs(dx) == radius && std::abs(dz) == radius;
                    if (corner && (random.NextInt(2) == 0 || y == 0)) continue;
                    WhisperwoodTryPlaceLeaf(level, origin + glm::ivec3(dx, y, dz), leavesBlock, leaves);
                }
            }
        }

        // TreeFeature.updateLeaves: within the tree's bounds, every block that
        // carries DISTANCE gets the length of its shortest six-neighbour path
        // to a log, 1..6 (7, the default, means "no log within reach"). A
        // multi-source BFS from the logs; the vanilla routine is the same
        // walk expressed as seven distance buckets. The engine has no leaf
        // decay yet, so today this only makes the saved state match what the
        // terrain library writes for a generated whisperwood.
        void WhisperwoodUpdateLeaves(ILevelWrite& level, const std::vector<glm::ivec3>& logs,
                                     const std::vector<glm::ivec3>& leaves) {
            if (logs.empty() || leaves.empty()) return;
            glm::ivec3 lo = logs[0], hi = logs[0];
            for (const auto& p : logs)   { lo = glm::min(lo, p); hi = glm::max(hi, p); }
            for (const auto& p : leaves) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
            auto inside = [&](const glm::ivec3& p) {
                return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y &&
                       p.z >= lo.z && p.z <= hi.z;
            };
            auto key = [&](const glm::ivec3& p) -> uint64_t {
                return (static_cast<uint64_t>(p.x - lo.x) << 42) |
                       (static_cast<uint64_t>(p.y - lo.y) << 21) |
                        static_cast<uint64_t>(p.z - lo.z);
            };
            constexpr Direction kAll[] = { Direction::Down, Direction::Up, Direction::North,
                                           Direction::South, Direction::West, Direction::East };

            std::unordered_set<uint64_t> seen;
            std::vector<glm::ivec3> frontier = logs;
            for (const auto& p : logs) seen.insert(key(p));
            for (int distance = 1; distance < 7 && !frontier.empty(); ++distance) {
                std::vector<glm::ivec3> next;
                for (const auto& p : frontier) {
                    for (Direction d : kAll) {
                        const glm::ivec3 n{p.x + StepX(d), p.y + StepY(d), p.z + StepZ(d)};
                        if (!inside(n) || !seen.insert(key(n)).second) continue;
                        const BlockState state = StateAt(level, n);
                        if (!state.HasProperty(PropertyId::DISTANCE)) continue;
                        // MC: min(current, distance) — a pre-existing leaf
                        // already closer to another tree's log keeps its value.
                        if (IntProperty(state, "distance") <= distance) { next.push_back(n); continue; }
                        // SetName, not StateWithInt: persistent/waterlogged
                        // must survive the write.
                        SetAt(level, n, state.SetName(PropertyId::DISTANCE, kDigits[distance]),
                              kTreeFlags);
                        next.push_back(n);
                    }
                }
                frontier.swap(next);
            }
        }

        // TreeFeature.place + doPlace for a straight-trunk blob configuration
        // (the whisperwood's, and every SaplingTreeSpec above).
        bool PlaceStraightBlobTree(ILevelWrite& level, const glm::ivec3& origin, JavaRandom& random,
                                   const SaplingTreeSpec& spec) {
            // TrunkPlacer.getTreeHeight:
            //   baseHeight + nextInt(heightRandA + 1) + nextInt(heightRandB + 1)
            // — the whisperwood's 5 + nextInt(3) + nextInt(2) is 5..8. Both
            // nextInt calls are made even for a zero rand (nextInt(1)), as in
            // vanilla, so the stream stays in step.
            const int treeHeight    = spec.baseHeight + random.NextInt(spec.heightRandA + 1) +
                                      random.NextInt(spec.heightRandB + 1);
            const int foliageHeight = spec.foliageHeight;   // BlobFoliagePlacer's fixed `height`
            const int leafRadius    = spec.leafRadius;      // FoliagePlacer.foliageRadius: ConstantInt

            // doPlace's build-height gate: minY >= level.minY + 1 and
            // maxY (= origin.y + treeHeight + 1) <= level.maxY + 1.
            if (origin.y < World::MIN_Y + 1 || origin.y + treeHeight + 1 > World::MAX_Y + 1) {
                return false;
            }
            // TwoLayersFeatureSize has no minClippedHeight, so any clipping
            // at all fails the tree — a whisperwood is never a stump.
            const int clipped = WhisperwoodMaxFreeTreeHeight(level, treeHeight, origin);
            if (clipped < treeHeight) return false;

            std::vector<glm::ivec3> logs, leaves;

            // StraightTrunkPlacer.placeTrunk: the soil, then one log per level
            // (TrunkPlacer.placeLog only writes into a valid tree position).
            WhisperwoodSetDirtBelow(level, origin - glm::ivec3(0, 1, 0));
            for (int y = 0; y < clipped; ++y) {
                const glm::ivec3 p = origin + glm::ivec3(0, y, 0);
                if (!WhisperwoodValidTreePos(level, p)) continue;
                SetAt(level, p, spec.log, kTreeFlags);   // axis y is the default
                logs.push_back(p);
            }

            // One FoliageAttachment at origin.above(clipped), radiusOffsetXZ 0,
            // single trunk. BlobFoliagePlacer.createFoliage with offset 0:
            //   for yo = 0 down to -(foliageHeight):
            //     r = max(leafRadius + 0 - 1 - yo / 2, 0)   (Java int division)
            // → rows of radius 1, 1, 2, 2 from the top down for radius 2
            //   (2, 2, 3, 3 for radius 3).
            const glm::ivec3 attach = origin + glm::ivec3(0, clipped, 0);
            for (int yo = 0; yo >= -foliageHeight; --yo) {
                const int r = std::max(leafRadius - 1 - yo / 2, 0);
                WhisperwoodPlaceLeavesRow(level, random, attach, r, yo, spec.leaves, leaves);
            }

            // TreeFeature.place: nothing written means the feature failed.
            if (logs.empty() && leaves.empty()) return false;
            WhisperwoodUpdateLeaves(level, logs, leaves);
            return true;
        }

        // TreeGrower.growTree: the sapling makes way (flag 4), the feature
        // runs, and if it could not place the sapling is put back unchanged.
        bool GrowWhisperwood(ILevelWrite& level, const glm::ivec3& pos, BlockState sapling,
                             JavaRandom& random) {
            const SaplingTreeSpec* spec = SaplingTreeSpecFor(sapling.Block());
            if (!spec) return false;
            SetAt(level, pos, BlockID::Air, kSaplingInvisible);
            if (PlaceStraightBlobTree(level, pos, random, *spec)) return true;
            SetAt(level, pos, sapling, kSaplingInvisible);
            return false;
        }

        // SaplingBlock.advanceTree: stage 0 → 1 (cycle), stage 1 → the tree.
        void AdvanceWhisperwood(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                JavaRandom& random) {
            if (StageOf(state) == 0) {
                SetAt(level, pos, StateWithInt(state.Block(), "stage", 1), kSaplingStageFlags);
                return;
            }
            GrowWhisperwood(level, pos, state, random);
        }

        bool SaplingIsRandomlyTicking(BlockState /*state*/) { return true; }

        // SaplingBlock.randomTick:
        //   if (level.getMaxLocalRawBrightness(pos.above()) >= 9
        //       && random.nextInt(7) == 0) advanceTree(...)
        // The light gate (BRIGHTNESS_FOR_SAPLING_GROWTH = 9) is deliberately
        // NOT ported for the whisperwood: the Hush is a fixed midnight
        // (docs/the-hush.md — time 18000, no sun) and the whisperwood is what
        // lights it, so a sapling that waited for daylight would never grow at
        // home. It grows regardless of light, anywhere. The 1-in-7 pacing
        // (TICK_CHANCE_FOR_SAPLING_GROWTH) is kept as is.
        void WhisperwoodSaplingRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                                          BlockState state, JavaRandom& random) {
            if (random.NextInt(7) != 0) return;
            AdvanceWhisperwood(level, pos, state, random);
        }

        // SaplingBlock.randomTick with its light gate — the Twilight Forest
        // and Aether saplings (TF/Aether register plain SaplingBlocks):
        //   getMaxLocalRawBrightness(pos.above()) >= 9 && nextInt(7) == 0
        // Brightness is the engine's sky-column stand-in (no block light), so
        // under the Twilight Forest's fixed dusk sky a sapling only grows
        // where the darkened sky still reads 9 — as in TF, where the same
        // gate makes torches the usual way to grow one.
        void LitSaplingRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                                  BlockState state, JavaRandom& random) {
            if (SpreadingMaxLocalRawBrightness(level, pos + glm::ivec3(0, 1, 0)) < 9) return;
            if (random.NextInt(7) != 0) return;
            AdvanceWhisperwood(level, pos, state, random);
        }

        // Aether BerryBushStemBlock.randomTick: in light >= 9, a 1-in-60
        // chance (CommonHooks.canCropGrow's default roll) to grow back into a
        // berry bush.
        bool BerryStemIsRandomlyTicking(BlockState /*state*/) { return true; }
        void BerryStemRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                                 BlockState /*state*/, JavaRandom& random) {
            if (SpreadingMaxLocalRawBrightness(level, pos + glm::ivec3(0, 1, 0)) < 9) return;
            if (random.NextInt(60) != 0) return;
            SetAt(level, pos, BlockID::BerryBush, kUpdateAll);   // setBlockAndUpdate
        }

        // BerryBushStemBlock: isValidBonemealTarget → true; isBonemealSuccess
        // → nextFloat() <= 0.45 (folded in here, on the level's random, as
        // the sapling does); performBonemeal → the bush. Server authority for
        // the same reason as the sapling: the roll is the server's.
        bool BerryStemIsValidBonemealTarget(const IBlockAccess& /*level*/, const glm::ivec3& /*pos*/,
                                            BlockState /*state*/) {
            return true;
        }
        void BerryStemPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos,
                                      BlockState /*state*/, JavaRandom& random) {
            if (level.IsClientSide()) return;
            JavaRandom* levelRandom = level.Random();
            JavaRandom& rng = levelRandom ? *levelRandom : random;
            if (!(rng.NextFloat() <= 0.45f)) return;
            SetAt(level, pos, BlockID::BerryBush, kUpdateAll);
        }

        // SaplingBlock.isValidBonemealTarget: treeGrower.canGrow (always true —
        // the whisperwood feature is fixed, not biome-picked) and
        // isInsideBuildHeight(pos.above(minimumHeight = 0)).
        bool SaplingIsValidBonemealTarget(const IBlockAccess& /*level*/, const glm::ivec3& pos,
                                          BlockState /*state*/) {
            return pos.y >= World::MIN_Y && pos.y <= World::MAX_Y;
        }

        // SaplingBlock.isBonemealSuccess (`level.getRandom().nextFloat() < 0.45`)
        // + performBonemeal (advanceTree). The engine has no isBonemealSuccess
        // hook — BoneMealItem's port folds it into the target test — so the
        // 45 % roll lives here, on the LEVEL's random as in vanilla: the
        // bone-meal item hands over a position-seeded stream, which would make
        // the roll a fixed property of the spot rather than a chance.
        void WhisperwoodSaplingPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos,
                                               BlockState state, JavaRandom& random) {
            // Server authority. The crop ports predict client-side because a
            // one-cell age step is cheap to be wrong about; a tree is sixty
            // blocks that would stay as ghosts if the server's roll failed.
            if (level.IsClientSide()) return;
            JavaRandom* levelRandom = level.Random();
            JavaRandom& rng = levelRandom ? *levelRandom : random;
            if (!(rng.NextFloat() < 0.45f)) return;
            AdvanceWhisperwood(level, pos, state, rng);
        }

    } // namespace

    // ────────────────────────────────────────────────────────────────────────
    // Registration
    // ────────────────────────────────────────────────────────────────────────
    void BlockRegistry_RegisterGrowth(std::array<Block, BlockRegistry::Size>& blocks) {
        int wired = 0;
        int unmatched = 0;

        // Matched by registrySlug, the same key BlockBehaviors.cpp uses — the
        // vanilla registry name, which is stable across model renames.
        auto forSlug = [&](std::string_view slug) -> Block* {
            for (auto& b : blocks) if (b.registrySlug == slug) return &b;
            ++unmatched;
            Log::Warning("[BlockGrowth] no block registered for slug '%.*s'",
                         static_cast<int>(slug.size()), slug.data());
            return nullptr;
        };

        auto wireTick = [&](std::string_view slug, BlockIsRandomlyTickingFn pred,
                            BlockRandomTickFn tick) {
            if (Block* b = forSlug(slug)) {
                b->isRandomlyTicking = pred;
                b->randomTick        = tick;
                ++wired;
            }
        };
        auto wireBonemeal = [&](std::string_view slug, BlockIsValidBonemealTargetFn valid,
                                BlockPerformBonemealFn perform) {
            if (Block* b = forSlug(slug)) {
                b->isValidBonemealTarget = valid;
                b->performBonemeal       = perform;
            }
        };

        // ── Crops on farmland ───────────────────────────────────────────────
        for (std::string_view slug : {"wheat", "carrots", "potatoes"}) {
            wireTick(slug, &CropIsRandomlyTicking, &CropRandomTick);
            wireBonemeal(slug, &CropIsValidBonemealTarget, &CropPerformBonemeal);
        }
        // Beetroot and torchflower share CropBlock but roll an extra 2-in-3
        // gate before attempting growth.
        for (std::string_view slug : {"beetroots", "torchflower_crop"}) {
            wireTick(slug, &CropIsRandomlyTicking, &SlowCropRandomTick);
            wireBonemeal(slug, &CropIsValidBonemealTarget, &CropPerformBonemeal);
        }

        // ── Stems ───────────────────────────────────────────────────────────
        for (std::string_view slug : {"melon_stem", "pumpkin_stem"}) {
            wireTick(slug, &CropIsRandomlyTicking, &StemRandomTick);
            wireBonemeal(slug, &StemIsValidBonemealTarget, &StemPerformBonemeal);
        }
        // Attached stems do not tick and are not bonemealable, but they DO
        // react to their fruit being picked by turning back into a stem at
        // age 7 — see AttachedStemUpdateShape.
        for (std::string_view slug : {"attached_melon_stem", "attached_pumpkin_stem"}) {
            if (Block* b = forSlug(slug)) {
                b->updateShape = &AttachedStemUpdateShape;
            }
        }

        // ── Nether wart — no bone meal in vanilla ───────────────────────────
        wireTick("nether_wart", &CropIsRandomlyTicking, &NetherWartRandomTick);

        // ── Pitcher crop ────────────────────────────────────────────────────
        wireTick("pitcher_crop", &PitcherIsRandomlyTicking, &PitcherRandomTick);
        wireBonemeal("pitcher_crop", &PitcherIsValidBonemealTarget, &PitcherPerformBonemeal);

        // ── Self-growing plants ─────────────────────────────────────────────
        wireTick("sugar_cane", &CropIsRandomlyTicking, &SugarCaneRandomTick);
        // Bedrock-style: bone meal grows a stalk 1-2 blocks. Java has no such
        // interaction — see the note on SugarCanePerformBonemeal.
        wireBonemeal("sugar_cane", &SugarCaneIsValidBonemealTarget,
                     &SugarCanePerformBonemeal);
        wireTick("cactus",     &CropIsRandomlyTicking, &CactusRandomTick);

        wireTick("grass_block", &SpreadingIsRandomlyTicking, &SpreadingRandomTick);
        wireTick("mycelium",    &SpreadingIsRandomlyTicking, &SpreadingRandomTick);
        // The Aether (pass one): AetherGrassBlock spreads onto aether dirt
        // exactly as grass does onto dirt — SpreadingBaseBlock picks the pair.
        wireTick("aether_grass_block", &SpreadingIsRandomlyTicking, &SpreadingRandomTick);

        wireTick("bamboo",         &BambooIsRandomlyTicking, &BambooRandomTick);
        wireBonemeal("bamboo",     &BambooIsValidBonemealTarget, &BambooPerformBonemeal);
        wireTick("bamboo_sapling", &CropIsRandomlyTicking, &BambooSaplingRandomTick);
        wireBonemeal("bamboo_sapling", &BambooSaplingIsValidBonemealTarget,
                     &BambooSaplingPerformBonemeal);

        wireTick("cocoa",     &CocoaIsRandomlyTicking, &CocoaRandomTick);
        wireBonemeal("cocoa", &CocoaIsValidBonemealTarget, &CocoaPerformBonemeal);

        wireTick("sweet_berry_bush",     &BerryIsRandomlyTicking, &BerryRandomTick);
        wireBonemeal("sweet_berry_bush", &BerryIsValidBonemealTarget, &BerryPerformBonemeal);

        // ── Farmland ────────────────────────────────────────────────────────
        wireTick("farmland", &FarmlandIsRandomlyTicking, &FarmlandRandomTick);

        // ── The Hush: whisperwood sapling ───────────────────────────────────
        // See the SaplingBlock section above; the mod saplings below share it.
        wireTick("whisperwood_sapling", &SaplingIsRandomlyTicking, &WhisperwoodSaplingRandomTick);
        wireBonemeal("whisperwood_sapling", &SaplingIsValidBonemealTarget,
                     &WhisperwoodSaplingPerformBonemeal);

        // ── Twilight Forest + The Aether saplings (pass one) ─────────────
        // The same placer, per-species (SaplingTreeSpecFor), with MC's
        // light gate on the random tick. Bone meal is ungated, as in MC.
        for (std::string_view slug : {"twilight_oak_sapling", "canopy_sapling",
                                      "tf_mangrove_sapling", "darkwood_sapling",
                                      "skyroot_sapling", "golden_oak_sapling"}) {
            wireTick(slug, &SaplingIsRandomlyTicking, &LitSaplingRandomTick);
            wireBonemeal(slug, &SaplingIsValidBonemealTarget, &WhisperwoodSaplingPerformBonemeal);
        }

        // ── The Aether: berry bush stem regrowth ─────────────────────────
        wireTick("berry_bush_stem", &BerryStemIsRandomlyTicking, &BerryStemRandomTick);
        wireBonemeal("berry_bush_stem", &BerryStemIsValidBonemealTarget, &BerryStemPerformBonemeal);

        Log::Info("[BlockGrowth] %d randomly-ticking blocks wired (%d slugs unmatched)",
                  wired, unmatched);
    }

} // namespace Game
