// File: src/common/world/block/PlantBlocks.cpp
//
// See PlantBlocks.hpp. Every function names the MC method it ports.
#include "common/world/block/PlantBlocks.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/lighting/BlockLightProperties.hpp"
#include "common/world/spawn/SpawnPlacements.hpp"
#include "common/world/tags/DataTags.hpp"

#include <mutex>

namespace Game {

    namespace {

        // One block tag resolved to a per-BlockID table on first use. The
        // data pack's tag files are the truth (DataTags), but DataTags takes a
        // lock and a string per question, and survival rules are asked on
        // every neighbour update — so each tag is walked once, over every
        // block, and read as a byte after that. Promoted state variants
        // (snowy grass) carry their base block's slug, so they inherit its
        // tags for free.
        class BlockTagTable {
        public:
            explicit BlockTagTable(const char* tag) : m_tag(tag) {}

            bool Has(BlockID id) {
                std::call_once(m_once, [this] {
                    for (size_t i = 0; i < BlockRegistry::Size; ++i) {
                        const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                        m_bits[i] = !slug.empty() &&
                                    DataTags::HasTag(DataTags::Registry::Block, slug, m_tag);
                    }
                });
                const size_t i = static_cast<size_t>(id);
                return i < m_bits.size() && m_bits[i];
            }

        private:
            const char*                               m_tag;
            std::once_flag                            m_once;
            std::array<bool, BlockRegistry::Size>     m_bits{};
        };

        BlockTagTable s_overridesMushroomLight("minecraft:overrides_mushroom_light_requirement");
        BlockTagTable s_hugeRedCanPlaceOn("minecraft:huge_red_mushroom_can_place_on");
        BlockTagTable s_hugeBrownCanPlaceOn("minecraft:huge_brown_mushroom_can_place_on");
        BlockTagTable s_replaceableByMushrooms("minecraft:replaceable_by_mushrooms");
        BlockTagTable s_leaves("minecraft:leaves");
        BlockTagTable s_supportsLilyPad("minecraft:supports_lily_pad");
        BlockTagTable s_supportsFrogspawn("minecraft:supports_frogspawn");

        // #minecraft:coral_blocks — the five live coral blocks.
        bool IsCoralBlock(BlockID id) {
            return id == BlockID::TubeCoralBlock || id == BlockID::BrainCoralBlock ||
                   id == BlockID::BubbleCoralBlock || id == BlockID::FireCoralBlock ||
                   id == BlockID::HornCoralBlock;
        }

        bool IsWaterlogged(BlockState state) {
            return state.HasProperty(PropertyId::WATERLOGGED) &&
                   state.GetName(PropertyId::WATERLOGGED) == "true";
        }

        // VegetationBlock.updateShape / SeaPickleBlock.updateShape /
        // FrogspawnBlock.updateShape: whichever neighbour changed, a plant
        // that can no longer survive is gone. (The waterlogged water tick the
        // pickle also books is the World's shared first line.)
        template <bool (*CanSurviveFn)(const IBlockAccess&, const glm::ivec3&)>
        bool SurviveOrBreakUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                       BlockState /*state*/, Direction /*toNeighbour*/,
                                       BlockID /*neighbourId*/, BlockState& outState,
                                       ScheduledTickAccess* /*ticks*/) {
            if (CanSurviveFn(level, pos)) return false;
            outState = BlockState{};
            return true;
        }

        // ── SeaPickleBlock bone meal ──────────────────────────────────────

        // isValidBonemealTarget: a live pickle on a coral block.
        bool SeaPickleIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                            BlockState state) {
            return IsWaterlogged(state) && IsCoralBlock(level.GetBlock(pos.x, pos.y - 1, pos.z));
        }

        // performBonemeal, verbatim: a diamond five wide around the pickle,
        // one and two blocks down from the cell above it, each water cell
        // over coral getting a 1-in-6 chance of a 1..4 pickle clump; then the
        // pickle itself fills to four.
        void SeaPicklePerformBonemeal(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                      JavaRandom& random) {
            int zSpan = 1;
            int count = 0;
            const int xStart = pos.x - 2;
            int zOffSet = 0;
            for (int x = 0; x < 5; ++x) {
                for (int z = 0; z < zSpan; ++z) {
                    const int endY = 2 + pos.y - 1;
                    for (int startY = endY - 2; startY < endY; ++startY) {
                        const glm::ivec3 position{xStart + x, startY, pos.z - zOffSet + z};
                        // `!position.equals(pos) && random.nextInt(6) == 0 && …`
                        // — the roll is only taken for the other cells.
                        if (position == pos) continue;
                        if (random.NextInt(6) != 0) continue;
                        if (level.GetBlock(position.x, position.y, position.z) != BlockID::Water) continue;
                        if (!IsCoralBlock(level.GetBlock(position.x, position.y - 1, position.z))) continue;
                        const BlockState clump = BlockStates::Default(BlockID::SeaPickle)
                                                     .SetIndex(PropertyId::PICKLES, random.NextInt(4));
                        level.SetBlock(position.x, position.y, position.z, clump, World::UpdateFlags::All);
                    }
                }
                if (count < 2) {
                    zSpan += 2;
                    ++zOffSet;
                } else {
                    zSpan -= 2;
                    --zOffSet;
                }
                ++count;
            }
            // level.setBlock(pos, state.setValue(PICKLES, 4), 2)
            level.SetBlock(pos.x, pos.y, pos.z, state.SetIndex(PropertyId::PICKLES, 3),
                           World::UpdateFlags::UpdateClients);
        }

        // ── TallGrassBlock (short grass, fern) bone meal ──────────────────

        // TallGrassBlock.getGrownBlock: short grass → tall grass, fern →
        // large fern.
        BlockID GrownDoublePlant(BlockID id) {
            if (id == BlockID::ShortGrass) return BlockID::TallGrass;
            if (id == BlockID::Fern)       return BlockID::LargeFern;
            return BlockID::Air;
        }

        // TallGrassBlock.isValidBonemealTarget: the double plant could stand
        // here and the cell above is empty and inside the build height.
        bool ShortPlantIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                             BlockState state) {
            const BlockID grown = GrownDoublePlant(state.Block());
            if (grown == BlockID::Air) return false;
            return CanSurviveAt(level, pos, grown) &&
                   level.IsValidPosition(pos.x, pos.y + 1, pos.z) &&
                   level.GetBlock(pos.x, pos.y + 1, pos.z) == BlockID::Air;
        }

        // TallGrassBlock.performBonemeal → DoublePlantBlock.placeAt(level,
        // grown.defaultBlockState(), pos, 2): the lower half here, the upper
        // above, both with UPDATE_CLIENTS.
        void ShortPlantPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                       JavaRandom& /*random*/) {
            const BlockID grown = GrownDoublePlant(state.Block());
            if (grown == BlockID::Air) return;
            const BlockState lower = BlockStates::Default(grown).SetName(PropertyId::DOUBLE_BLOCK_HALF, "lower");
            const BlockState upper = lower.SetName(PropertyId::DOUBLE_BLOCK_HALF, "upper");
            level.SetBlock(pos.x, pos.y, pos.z, lower, World::UpdateFlags::UpdateClients);
            level.SetBlock(pos.x, pos.y + 1, pos.z, upper, World::UpdateFlags::UpdateClients);
        }

        // ── GrassBlock bone meal ──────────────────────────────────────────

        // GrassBlock.isValidBonemealTarget: air above, inside the build height.
        bool GrassIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                        BlockState /*state*/) {
            return level.IsValidPosition(pos.x, pos.y + 1, pos.z) &&
                   level.GetBlock(pos.x, pos.y + 1, pos.z) == BlockID::Air;
        }

        // SimpleBlockFeature.place for one plant state: it must survive where
        // it goes, and it is written with UPDATE_CLIENTS.
        void PlaceSimplePlant(ILevelWrite& level, const glm::ivec3& p, BlockID plant) {
            if (!CanSurviveAt(level, p, plant)) return;
            level.SetBlock(p.x, p.y, p.z, BlockStates::Default(plant), World::UpdateFlags::UpdateClients);
        }

        // GrassBlock.placeBonemealEffect.
        void GrassPlaceBonemealEffect(ILevelWrite& level, JavaRandom& random, const glm::ivec3& testPos) {
            const BlockState testState = level.GetBlockState(testPos.x, testPos.y, testPos.z);
            // Short grass already there: a one-in-ten chance to grow it tall.
            if (testState.Is(BlockID::ShortGrass) && random.NextFloat() < 0.1f) {
                if (ShortPlantIsValidBonemealTarget(level, testPos, testState)) {
                    ShortPlantPerformBonemeal(level, testPos, testState, random);
                }
            }
            if (testState.Block() != BlockID::Air || !level.IsValidPosition(testPos.x, testPos.y, testPos.z)) {
                return;
            }
            if (random.NextFloat() < 0.125f) {
                // Biome.getGenerationSettings().getBoneMealFeatures(), one at
                // random. The game world has no biome feature lists, so this
                // is the overworld default flower feature (flower_default: a
                // poppy twice as often as a dandelion), placed as one plant.
                const BlockID flower = random.NextInt(3) == 0 ? BlockID::Dandelion : BlockID::Poppy;
                PlaceSimplePlant(level, testPos, flower);
            } else {
                // VegetationPlacements.GRASS_BONEMEAL: a short grass, only
                // into an empty cell (onlyWhenEmpty).
                PlaceSimplePlant(level, testPos, BlockID::ShortGrass);
            }
        }

        // GrassBlock.performBonemeal: 128 attempts, each a random walk from
        // the cell above that stops at the first step off the grass or into a
        // full-collision block, then the effect where it ended.
        void GrassPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos, BlockState /*state*/,
                                  JavaRandom& random) {
            const glm::ivec3 above = pos + glm::ivec3(0, 1, 0);
            for (int attempt = 0; attempt < 128; ++attempt) {
                glm::ivec3 testPos = above;
                const int randomizeCount = attempt / 16;
                bool stopped = false;
                for (int i = 0; i < randomizeCount; ++i) {
                    // nextIntBetweenInclusive(-1, 1), Java's argument order.
                    const int dx = random.NextInt(-1, 1);
                    const int dyBase = random.NextInt(-1, 1);
                    const int dy = dyBase * random.NextInt(3) / 2;
                    const int dz = random.NextInt(-1, 1);
                    testPos += glm::ivec3(dx, dy, dz);
                    // stopBonemealSpread: no grass block under the step, or
                    // the step lands in a full-collision block.
                    if (level.GetBlock(testPos.x, testPos.y - 1, testPos.z) != BlockID::Grass ||
                        IsCollisionShapeFullBlock(level, testPos.x, testPos.y, testPos.z)) {
                        stopped = true;
                        break;
                    }
                }
                if (stopped) continue;
                GrassPlaceBonemealEffect(level, random, testPos);
            }
        }

        // ── MushroomBlock ─────────────────────────────────────────────────

        // The two features' constants (TreeFeatures.HUGE_*_MUSHROOM).
        struct HugeMushroomFeature {
            bool          red;              // HugeRedMushroomFeature vs HugeBrownMushroomFeature
            BlockID       cap;
            int           foliageRadius;
            BlockTagTable* canPlaceOn;
        };
        const HugeMushroomFeature kHugeRed{true, BlockID::RedMushroomBlock, 2, &s_hugeRedCanPlaceOn};
        const HugeMushroomFeature kHugeBrown{false, BlockID::BrownMushroomBlock, 3, &s_hugeBrownCanPlaceOn};

        const HugeMushroomFeature* FeatureFor(BlockID mushroom) {
            if (mushroom == BlockID::RedMushroom)   return &kHugeRed;
            if (mushroom == BlockID::BrownMushroom) return &kHugeBrown;
            return nullptr;
        }

        BlockState WithFlag(BlockState s, PropertyId prop, bool value) {
            return s.SetName(prop, value ? "true" : "false");
        }

        // AbstractHugeMushroomFeature.getTreeHeight.
        int TreeHeight(JavaRandom& random) {
            int treeHeight = random.NextInt(3) + 4;
            if (random.NextInt(12) == 0) treeHeight *= 2;
            return treeHeight;
        }

        // getTreeRadiusForHeight(-1, -1, foliageRadius, dy) — the arguments
        // isValidPosition really passes. For the red feature that makes every
        // radius 0 (dy is never below -1 nor equal to it), so only the column
        // itself is checked: vanilla's own quirk, kept.
        int TreeRadiusForHeight(const HugeMushroomFeature& f, int dy) {
            if (f.red) {
                constexpr int treeHeight = -1;
                if (dy < treeHeight && dy >= treeHeight - 3) return f.foliageRadius;
                if (dy == treeHeight) return f.foliageRadius;
                return 0;
            }
            return dy <= 3 ? 0 : f.foliageRadius;
        }

        // AbstractHugeMushroomFeature.isValidPosition.
        bool IsValidPosition(ILevelWrite& level, const HugeMushroomFeature& f,
                             const glm::ivec3& origin, int treeHeight) {
            // y >= level.getMinY() + 1 && y + treeHeight + 1 <= level.getMaxY()
            if (!level.IsValidPosition(origin.x, origin.y - 1, origin.z)) return false;
            if (!level.IsValidPosition(origin.x, origin.y + treeHeight + 1, origin.z)) return false;
            if (!f.canPlaceOn->Has(level.GetBlock(origin.x, origin.y - 1, origin.z))) return false;
            for (int dy = 0; dy <= treeHeight; ++dy) {
                const int radius = TreeRadiusForHeight(f, dy);
                for (int dx = -radius; dx <= radius; ++dx) {
                    for (int dz = -radius; dz <= radius; ++dz) {
                        const BlockID id = level.GetBlock(origin.x + dx, origin.y + dy, origin.z + dz);
                        if (id != BlockID::Air && !s_leaves.Has(id)) return false;
                    }
                }
            }
            return true;
        }

        // AbstractHugeMushroomFeature.placeMushroomBlock: only into air or
        // #replaceable_by_mushrooms, through Feature.setBlock (flags 3).
        void PlaceMushroomBlock(ILevelWrite& level, const glm::ivec3& p, BlockState newState) {
            const BlockID current = level.GetBlock(p.x, p.y, p.z);
            if (current == BlockID::Air || s_replaceableByMushrooms.Has(current)) {
                level.SetBlock(p.x, p.y, p.z, newState, World::UpdateFlags::All);
            }
        }

        // HugeRedMushroomFeature.makeCap. The cap provider is
        // red_mushroom_block with DOWN false (every other face true).
        void MakeRedCap(ILevelWrite& level, const HugeMushroomFeature& f,
                        const glm::ivec3& origin, int treeHeight) {
            const BlockState capBase =
                WithFlag(BlockStates::Default(f.cap), PropertyId::DOWN, false);
            for (int dy = treeHeight - 3; dy <= treeHeight; ++dy) {
                const int radius = dy < treeHeight ? f.foliageRadius : f.foliageRadius - 1;
                const int center = f.foliageRadius - 2;
                for (int dx = -radius; dx <= radius; ++dx) {
                    for (int dz = -radius; dz <= radius; ++dz) {
                        const bool xEdge = dx == -radius || dx == radius;
                        const bool zEdge = dz == -radius || dz == radius;
                        if (!(dy >= treeHeight || xEdge != zEdge)) continue;
                        BlockState s = capBase;
                        s = WithFlag(s, PropertyId::UP,    dy >= treeHeight - 1);
                        s = WithFlag(s, PropertyId::WEST,  dx < -center);
                        s = WithFlag(s, PropertyId::EAST,  dx > center);
                        s = WithFlag(s, PropertyId::NORTH, dz < -center);
                        s = WithFlag(s, PropertyId::SOUTH, dz > center);
                        PlaceMushroomBlock(level, origin + glm::ivec3(dx, dy, dz), s);
                    }
                }
            }
        }

        // HugeBrownMushroomFeature.makeCap. The cap provider is
        // brown_mushroom_block with UP true and DOWN false.
        void MakeBrownCap(ILevelWrite& level, const HugeMushroomFeature& f,
                          const glm::ivec3& origin, int treeHeight) {
            const int r = f.foliageRadius;
            const BlockState capBase = WithFlag(WithFlag(BlockStates::Default(f.cap),
                                                         PropertyId::UP, true),
                                                PropertyId::DOWN, false);
            for (int dx = -r; dx <= r; ++dx) {
                for (int dz = -r; dz <= r; ++dz) {
                    const bool minX = dx == -r, maxX = dx == r;
                    const bool minZ = dz == -r, maxZ = dz == r;
                    const bool xEdge = minX || maxX;
                    const bool zEdge = minZ || maxZ;
                    if (xEdge && zEdge) continue;   // the four corners stay open
                    const bool west  = minX || (zEdge && dx == 1 - r);
                    const bool east  = maxX || (zEdge && dx == r - 1);
                    const bool north = minZ || (xEdge && dz == 1 - r);
                    const bool south = maxZ || (xEdge && dz == r - 1);
                    BlockState s = capBase;
                    s = WithFlag(s, PropertyId::WEST,  west);
                    s = WithFlag(s, PropertyId::EAST,  east);
                    s = WithFlag(s, PropertyId::NORTH, north);
                    s = WithFlag(s, PropertyId::SOUTH, south);
                    PlaceMushroomBlock(level, origin + glm::ivec3(dx, treeHeight, dz), s);
                }
            }
        }

        // AbstractHugeMushroomFeature.placeTrunk: the stem, mushroom_stem with
        // UP and DOWN false.
        void PlaceTrunk(ILevelWrite& level, const glm::ivec3& origin, int treeHeight) {
            const BlockState stem = WithFlag(WithFlag(BlockStates::Default(BlockID::MushroomStem),
                                                      PropertyId::UP, false),
                                             PropertyId::DOWN, false);
            for (int dy = 0; dy < treeHeight; ++dy) {
                PlaceMushroomBlock(level, origin + glm::ivec3(0, dy, 0), stem);
            }
        }

        // AbstractHugeMushroomFeature.place.
        bool PlaceHugeMushroom(ILevelWrite& level, const HugeMushroomFeature& f,
                               const glm::ivec3& origin, JavaRandom& random) {
            const int treeHeight = TreeHeight(random);
            if (!IsValidPosition(level, f, origin, treeHeight)) return false;
            if (f.red) MakeRedCap(level, f, origin, treeHeight);
            else       MakeBrownCap(level, f, origin, treeHeight);
            PlaceTrunk(level, origin, treeHeight);
            return true;
        }

        bool MushroomIsRandomlyTicking(BlockState /*state*/) { return true; }

        // MushroomBlock.canSpread: at most four of this mushroom (itself
        // included) in the 9x3x9 box around it.
        bool MushroomCanSpread(const IBlockAccess& level, const glm::ivec3& pos, BlockID self) {
            int found = 0;
            for (int x = pos.x - 4; x <= pos.x + 4; ++x) {
                for (int y = pos.y - 1; y <= pos.y + 1; ++y) {
                    for (int z = pos.z - 4; z <= pos.z + 4; ++z) {
                        if (level.GetBlock(x, y, z) == self && ++found > 4) return false;
                    }
                }
            }
            return true;
        }

        // MushroomBlock.randomTick: one in 25 ticks, a short random walk from
        // the mushroom to an empty cell it could live in.
        void MushroomRandomTick(ILevelWrite& level, const glm::ivec3& start, BlockState state,
                                JavaRandom& random) {
            if (random.NextInt(25) != 0) return;
            const BlockID self = state.Block();
            if (!MushroomCanSpread(level, start, self)) return;

            // pos.offset(nextInt(3) - 1, nextInt(2) - nextInt(2), nextInt(3) - 1),
            // Java's left-to-right argument order made explicit.
            auto randomOffset = [&random](const glm::ivec3& from) {
                const int dx = random.NextInt(3) - 1;
                const int a  = random.NextInt(2);
                const int b  = random.NextInt(2);
                const int dz = random.NextInt(3) - 1;
                return from + glm::ivec3(dx, a - b, dz);
            };
            auto isEmpty = [&level](const glm::ivec3& p) {
                return level.IsValidPosition(p.x, p.y, p.z) &&
                       level.GetBlock(p.x, p.y, p.z) == BlockID::Air;
            };

            glm::ivec3 pos = start;
            glm::ivec3 offset = randomOffset(pos);
            for (int i = 0; i < 4; ++i) {
                if (isEmpty(offset) && MushroomCanSurvive(level, offset)) pos = offset;
                offset = randomOffset(pos);
            }
            if (isEmpty(offset) && MushroomCanSurvive(level, offset)) {
                // level.setBlock(offset, state, 2)
                level.SetBlock(offset.x, offset.y, offset.z, state, World::UpdateFlags::UpdateClients);
            }
        }

        // MushroomBlock.isValidBonemealTarget: the feature's minimum height
        // (4 + foliage radius) above the mushroom is inside the build height.
        // Vanilla answers false on a client level; here the client may say
        // yes, which only makes its bone-meal click report success — the
        // growth itself only ever runs on the server (UseOn_BoneMeal).
        bool MushroomIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                           BlockState state) {
            const HugeMushroomFeature* f = FeatureFor(state.Block());
            if (!f) return false;
            const int minHeight = 4 + f->foliageRadius;
            return level.IsValidPosition(pos.x, pos.y + minHeight, pos.z);
        }

        // MushroomBlock.isBonemealSuccess.
        bool MushroomIsBonemealSuccess(ILevelWrite& /*level*/, const glm::ivec3& /*pos*/,
                                       BlockState /*state*/, JavaRandom& random) {
            return static_cast<double>(random.NextFloat()) < 0.4;
        }

        void MushroomPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                     JavaRandom& random) {
            GrowHugeMushroom(level, pos, state, random);
        }

    } // namespace

    // ── Public surface ────────────────────────────────────────────────────

    int SeaPickle::Pickles(BlockState state) {
        if (!state.Is(BlockID::SeaPickle)) return 0;
        const int index = state.GetIndex(PropertyId::PICKLES);   // values run 1..4
        return index < 0 ? 0 : index + 1;
    }

    bool SeaPickle::CanSurvive(const IBlockAccess& level, const glm::ivec3& pos) {
        // mayPlaceOn(below): !collisionShape.getFaceShape(UP).isEmpty()
        //                    || isFaceSturdy(level, below, UP)
        const glm::ivec3 below{pos.x, pos.y - 1, pos.z};
        const BlockState state = level.GetBlockState(below.x, below.y, below.z);
        const BlockID id = state.Block();
        if (id == BlockID::Air) return false;
        if (BlockRegistry::HasCollision(id)) {
            // getFaceShape(UP) is the slice at y = 0.9999999.
            constexpr float kSlice = 0.9999999f;
            for (const auto& box : BlockRegistry::GetBlockCollisionShapeSet(state)) {
                if (box.min.y <= kSlice && box.max.y >= kSlice &&
                    box.max.x > box.min.x && box.max.z > box.min.z) {
                    return true;
                }
            }
        }
        return IsFaceSturdyAt(level, below, Direction::Up);
    }

    bool LilyPadCanSurvive(const IBlockAccess& level, const glm::ivec3& pos) {
        // mayPlaceOn(stateBelow, level, below):
        //   (level.getFluidState(below).is(FluidTags.SUPPORTS_LILY_PAD)
        //    || stateBelow.is(BlockTags.SUPPORTS_LILY_PAD))
        //   && level.getFluidState(below.above()).is(Fluids.EMPTY)
        // #supports_lily_pad (fluid) is minecraft:water — the SOURCE fluid.
        const glm::ivec3 below{pos.x, pos.y - 1, pos.z};
        const bool supported =
            GetFluidState(level, below).IsSourceOf(FluidType::Water) ||
            s_supportsLilyPad.Has(level.GetBlock(below.x, below.y, below.z));
        return supported && GetFluidState(level, pos).IsEmpty();
    }

    bool FrogspawnCanSurvive(const IBlockAccess& level, const glm::ivec3& pos) {
        // FrogspawnBlock.mayPlaceOn(level, below): #supports_frogspawn fluid
        // (minecraft:water, the source) or block (none in vanilla), and an
        // empty fluid above it — the frogspawn's own cell.
        const glm::ivec3 below{pos.x, pos.y - 1, pos.z};
        const bool supported =
            GetFluidState(level, below).IsSourceOf(FluidType::Water) ||
            s_supportsFrogspawn.Has(level.GetBlock(below.x, below.y, below.z));
        return supported && GetFluidState(level, pos).IsEmpty();
    }

    bool IsSmallMushroom(BlockID id) {
        return id == BlockID::RedMushroom || id == BlockID::BrownMushroom;
    }

    bool MushroomCanSurvive(const IBlockAccess& level, const glm::ivec3& pos) {
        const BlockState below = level.GetBlockState(pos.x, pos.y - 1, pos.z);
        if (s_overridesMushroomLight.Has(below.Block())) return true;
        // DEVIATION (user request): 26.3-pre-2 also requires
        // level.getRawBrightness(pos, 0) < 13 here; newer MC dropped the
        // darkness rule, so only mayPlaceOn(below) — `state.isSolidRender()`
        // for a mushroom — remains. Placement, survival, spread and bone
        // meal all go through this check.
        return Lighting::BlockLightProperties::SolidRender(below);
    }

    bool GrowHugeMushroom(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                          JavaRandom& random) {
        const HugeMushroomFeature* f = FeatureFor(state.Block());
        if (!f) return false;
        // level.removeBlock(pos, false)
        level.SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);
        if (PlaceHugeMushroom(level, *f, pos, random)) return true;
        // level.setBlockAndUpdate(pos, state)
        level.SetBlock(pos.x, pos.y, pos.z, state, World::UpdateFlags::All);
        return false;
    }

    void RegisterPlantBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        // GrassBlock and TallGrassBlock (short grass, fern) are BonemealableBlocks
        // in vanilla: bone meal goes through growCrop like any crop, and the
        // growth-particle event finds them as bonemealable on the client.
        {
            Block& grass = blocks[static_cast<size_t>(BlockID::Grass)];
            grass.isValidBonemealTarget = &GrassIsValidBonemealTarget;
            grass.performBonemeal       = &GrassPerformBonemeal;
            grass.bonemealServerOnly    = true;
            for (BlockID id : {BlockID::ShortGrass, BlockID::Fern}) {
                Block& b = blocks[static_cast<size_t>(id)];
                b.isValidBonemealTarget = &ShortPlantIsValidBonemealTarget;
                b.performBonemeal       = &ShortPlantPerformBonemeal;
            }
        }

        Block& pickle = blocks[static_cast<size_t>(BlockID::SeaPickle)];
        pickle.updateShape           = &SurviveOrBreakUpdateShape<&SeaPickle::CanSurvive>;
        pickle.isValidBonemealTarget = &SeaPickleIsValidBonemealTarget;
        pickle.performBonemeal       = &SeaPicklePerformBonemeal;
        pickle.bonemealServerOnly    = true;

        blocks[static_cast<size_t>(BlockID::LilyPad)].updateShape =
            &SurviveOrBreakUpdateShape<&LilyPadCanSurvive>;
        blocks[static_cast<size_t>(BlockID::Frogspawn)].updateShape =
            &SurviveOrBreakUpdateShape<&FrogspawnCanSurvive>;

        for (BlockID id : {BlockID::RedMushroom, BlockID::BrownMushroom}) {
            Block& m = blocks[static_cast<size_t>(id)];
            m.updateShape           = &SurviveOrBreakUpdateShape<&MushroomCanSurvive>;
            m.isRandomlyTicking     = &MushroomIsRandomlyTicking;
            m.randomTick            = &MushroomRandomTick;
            m.isValidBonemealTarget = &MushroomIsValidBonemealTarget;
            m.isBonemealSuccess     = &MushroomIsBonemealSuccess;
            m.performBonemeal       = &MushroomPerformBonemeal;
            m.bonemealServerOnly    = true;
        }
    }

} // namespace Game
