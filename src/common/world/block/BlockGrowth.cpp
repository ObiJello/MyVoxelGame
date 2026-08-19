// File: src/common/world/block/BlockGrowth.cpp
//
// Per-block growth callbacks — the third file in the same family as
// BlockBehaviors.cpp (right-click reactions) and BlockPlacement.cpp (placement
// rules). BlockRegistry::Init calls BlockRegistry_RegisterGrowth once the block
// table exists, and this file fills in the `isRandomlyTicking` / `randomTick` /
// `isValidBonemealTarget` / `performBonemeal` function pointers.
//
// Every entry is a port of a BlockBehaviour subclass from
// minecraft_code/decompiled_net/minecraft/world/level/block/. The constants
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
#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

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
        bool AttachedStemNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockState state,
                                         Direction toNeighbour, BlockID neighbourId,
                                         BlockState& outState) {
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
        // age 7 — see AttachedStemNeighborChanged.
        for (std::string_view slug : {"attached_melon_stem", "attached_pumpkin_stem"}) {
            if (Block* b = forSlug(slug)) {
                b->neighborChanged = &AttachedStemNeighborChanged;
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

        Log::Info("[BlockGrowth] %d randomly-ticking blocks wired (%d slugs unmatched)",
                  wired, unmatched);
    }

} // namespace Game
