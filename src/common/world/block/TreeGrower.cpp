// File: src/common/world/block/TreeGrower.cpp
//
// See TreeGrower.hpp. Every function names the MC method it ports; the
// constants (9, 7, 0.45, 0.4, the weights) are MC's.
#include "common/world/block/TreeGrower.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/tags/DataTags.hpp"

#include <mutex>
#include <optional>
#include <utility>

namespace Game {

    // ── LiveFeatures hooks ──────────────────────────────────────────────────

    namespace LiveFeatures {

        namespace {
            Hooks& Storage() {
                static Hooks s_hooks;
                return s_hooks;
            }
        }

        void SetHooks(const Hooks& hooks) { Storage() = hooks; }
        const Hooks& GetHooks() { return Storage(); }

        bool HasFeature(std::string_view featureId) {
            const Hooks& h = Storage();
            return h.hasFeature && h.hasFeature(featureId);
        }

        bool Place(ILevelWrite& level, std::string_view featureId, const glm::ivec3& origin,
                   JavaRandom& random) {
            const Hooks& h = Storage();
            if (!h.place || level.IsClientSide()) return false;
            return h.place(level, featureId, origin, random);
        }

        void SendBlockUpdated(ILevelWrite& level, const glm::ivec3& pos) {
            const Hooks& h = Storage();
            if (h.sendBlockUpdated && !level.IsClientSide()) h.sendBlockUpdated(level, pos);
        }

    } // namespace LiveFeatures

    namespace {

        // ── MC update flags used by the growers ─────────────────────────────
        using UF = World::UpdateFlags;
        // SaplingBlock.advanceTree's stage step and TreeGrower.resetSaplings:
        // 260 = UPDATE_SKIP_BLOCK_ENTITY_SIDEEFFECTS | UPDATE_INVISIBLE.
        constexpr uint32_t kFlags260 = UF::SkipBlockEntitySideEffects | UF::Invisible;
        // TreeGrower.removeSapling: 818 = UPDATE_SKIP_ON_PLACE |
        // UPDATE_SKIP_BLOCK_ENTITY_SIDEEFFECTS | UPDATE_SUPPRESS_DROPS |
        // UPDATE_KNOWN_SHAPE | UPDATE_CLIENTS.
        constexpr uint32_t kFlags818 = UF::SkipOnPlace | UF::SkipBlockEntitySideEffects |
                                       UF::SuppressDrops | UF::KnownShape | UF::UpdateClients;
        // The 1.21.1 TreeGrower / SaplingBlock (the mods): UPDATE_INVISIBLE.
        constexpr uint32_t kFlags4 = UF::Invisible;
        // MangrovePropaguleBlock's age step: UPDATE_CLIENTS.
        constexpr uint32_t kFlags2 = UF::UpdateClients;

        // ── Weighted feature lists (MC WeightedList<ResourceKey<Feature>>) ─

        struct Weighted {
            std::string_view id;
            int              weight;
        };

        struct WeightedList {
            std::array<Weighted, 3> entries{};
            int                     count = 0;

            constexpr bool Empty() const { return count == 0; }
            constexpr int TotalWeight() const {
                int total = 0;
                for (int i = 0; i < count; ++i) total += entries[i].weight;
                return total;
            }
        };

        constexpr WeightedList None() { return {}; }
        constexpr WeightedList One(std::string_view id) { return {{{{id, 1}}}, 1}; }
        constexpr WeightedList Two(std::string_view a, int wa, std::string_view b, int wb) {
            return {{{{a, wa}, {b, wb}}}, 2};
        }
        constexpr WeightedList Three(std::string_view a, int wa, std::string_view b, int wb,
                                     std::string_view c, int wc) {
            return {{{{a, wa}, {b, wb}, {c, wc}}}, 3};
        }

        // WeightedList.getRandom: no draw for an empty list, otherwise one
        // nextInt(totalWeight) resolved by walking the cumulative weights —
        // what both the Flat (< 64) and Compact selectors answer.
        std::optional<std::string_view> Pick(const WeightedList& list, JavaRandom& random) {
            if (list.Empty()) return std::nullopt;
            int selection = random.NextInt(list.TotalWeight());
            for (int i = 0; i < list.count; ++i) {
                if (selection < list.entries[i].weight) return list.entries[i].id;
                selection -= list.entries[i].weight;
            }
            return list.entries[list.count - 1].id;   // unreachable: weights sum to the bound
        }

        // ── Growers ─────────────────────────────────────────────────────────

        enum class GrowerKind : uint8_t {
            Modern,   // 26.3 TreeGrower (vanilla, the Hush)
            Legacy,   // 1.21.1 TreeGrower (the Twilight Forest, the Aether)
        };

        struct TreeGrowerDef {
            GrowerKind kind;
            // Modern
            WeightedList trees;
            WeightedList megaTrees;
            WeightedList flowerTrees;
            // Legacy
            float            secondaryChance = 0.0f;
            std::string_view megaTree;
            std::string_view secondaryMegaTree;
            std::string_view tree;
            std::string_view secondaryTree;
            std::string_view flowers;
            std::string_view secondaryFlowers;
            // getMinimumHeight: the shortest tree's trunkPlacer().getBaseHeight(),
            // -1 for OptionalInt.empty() (dark oak, pale oak).
            int minimumHeight = -1;
        };

        constexpr TreeGrowerDef Modern(WeightedList trees, WeightedList mega, WeightedList flowerTrees,
                                       int minimumHeight) {
            TreeGrowerDef g{GrowerKind::Modern, trees, mega, flowerTrees};
            g.minimumHeight = minimumHeight;
            return g;
        }

        // TreeGrower(name, secondaryChance, megaTree, secondaryMegaTree, tree,
        // secondaryTree, flowers, secondaryFlowers) — 1.21.1. An empty view is
        // Optional.empty().
        constexpr TreeGrowerDef Legacy(float secondaryChance, std::string_view mega,
                                       std::string_view secondaryMega, std::string_view tree,
                                       std::string_view secondaryTree) {
            TreeGrowerDef g{GrowerKind::Legacy, None(), None(), None()};
            g.secondaryChance   = secondaryChance;
            g.megaTree          = mega;
            g.secondaryMegaTree = secondaryMega;
            g.tree              = tree;
            g.secondaryTree     = secondaryTree;
            return g;
        }

        // TreeGrower.java static block (26.3-pre-2). The minimum heights are
        // each shortestTreeType's trunk placer base height:
        //   oak StraightTrunkPlacer(4,2,0) 4 · spruce (5,2,1) 5 · mangrove
        //   UpwardsBranchingTrunkPlacer(2,1,4) 2 · azalea BendingTrunkPlacer
        //   (4,2,0) 4 · birch (5,2,0) 5 · jungle_tree_no_vine (4,8,0) 4 ·
        //   acacia ForkingTrunkPlacer(5,2,2) 5 · cherry CherryTrunkPlacer
        //   (7,1,0) 7 · red_poplar PoplarTrunkPlacer(7,4,0) 7.
        const TreeGrowerDef kOak = Modern(
            Two("minecraft:oak", 9, "minecraft:fancy_oak", 1), None(),
            Two("minecraft:oak_bees_005", 9, "minecraft:fancy_oak_bees_005", 1), 4);
        const TreeGrowerDef kSpruce = Modern(
            One("minecraft:spruce"),
            Two("minecraft:mega_spruce", 1, "minecraft:mega_pine", 1), None(), 5);
        const TreeGrowerDef kMangrove = Modern(
            Two("minecraft:mangrove", 15, "minecraft:tall_mangrove", 85), None(), None(), 2);
        const TreeGrowerDef kAzalea = Modern(One("minecraft:azalea_tree"), None(), None(), 4);
        const TreeGrowerDef kBirch = Modern(
            One("minecraft:birch"), None(), One("minecraft:birch_bees_005"), 5);
        const TreeGrowerDef kJungle = Modern(
            One("minecraft:jungle_tree_no_vine"), One("minecraft:mega_jungle_tree"), None(), 4);
        const TreeGrowerDef kAcacia = Modern(One("minecraft:acacia"), None(), None(), 5);
        const TreeGrowerDef kCherry = Modern(
            One("minecraft:cherry"), None(), One("minecraft:cherry_bees_005"), 7);
        const TreeGrowerDef kDarkOak = Modern(None(), One("minecraft:dark_oak"), None(), -1);
        const TreeGrowerDef kPaleOak = Modern(None(), One("minecraft:pale_oak_bonemeal"), None(), -1);
        const TreeGrowerDef kPoplar = Modern(
            Three("minecraft:red_poplar", 1, "minecraft:orange_poplar", 1,
                  "minecraft:yellow_poplar", 1), None(), None(), 7);

        // The Hush: the whisperwood (HushFeatures WHISPERWOOD, StraightTrunk
        // Placer(5, 2, 1)) on a 26.3 grower.
        const TreeGrowerDef kWhisperwood = Modern(One("hush:whisperwood"), None(), None(), 5);

        // TFTreeGrowers.java (the Twilight Forest, 1.21.1).
        const TreeGrowerDef kTwilightOak = Legacy(0.1f,
            "twilightforest:tree/forest_mega_oak_tree", "twilightforest:tree/savannah_mega_oak_tree",
            "twilightforest:tree/twilight_oak_tree", "twilightforest:tree/large_twilight_oak_tree");
        const TreeGrowerDef kCanopy = Legacy(0.0f,
            "twilightforest:tree/mega_canopy_tree", {}, "twilightforest:tree/canopy_tree", {});
        const TreeGrowerDef kTfMangrove = Legacy(0.0f, {}, {}, "twilightforest:tree/mangrove_tree", {});
        const TreeGrowerDef kDarkwood = Legacy(0.0f, {}, {},
            "twilightforest:tree/homegrown_darkwood_tree", {});
        // AetherTreeGrowers.java (the Aether, 1.21.1).
        const TreeGrowerDef kSkyroot = Legacy(0.0f, {}, {}, "aether:skyroot_tree", {});
        const TreeGrowerDef kGoldenOak = Legacy(0.0f, {}, {}, "aether:golden_oak_tree", {});

        const TreeGrowerDef* GrowerFor(BlockID id) {
            switch (id) {
                case BlockID::OakSapling:         return &kOak;
                case BlockID::SpruceSapling:      return &kSpruce;
                case BlockID::BirchSapling:       return &kBirch;
                case BlockID::JungleSapling:      return &kJungle;
                case BlockID::AcaciaSapling:      return &kAcacia;
                case BlockID::CherrySapling:      return &kCherry;
                case BlockID::DarkOakSapling:     return &kDarkOak;
                case BlockID::PaleOakSapling:     return &kPaleOak;
                case BlockID::PoplarSapling:      return &kPoplar;
                case BlockID::MangrovePropagule:  return &kMangrove;
                case BlockID::Azalea:
                case BlockID::FloweringAzalea:    return &kAzalea;
                case BlockID::WhisperwoodSapling: return &kWhisperwood;
                case BlockID::TwilightOakSapling: return &kTwilightOak;
                case BlockID::CanopySapling:      return &kCanopy;
                case BlockID::TfMangroveSapling:  return &kTfMangrove;
                case BlockID::DarkwoodSapling:    return &kDarkwood;
                case BlockID::SkyrootSapling:     return &kSkyroot;
                case BlockID::GoldenOakSapling:   return &kGoldenOak;
                default:                          return nullptr;
            }
        }

        // ── Level helpers ───────────────────────────────────────────────────

        BlockState StateAt(const IBlockAccess& level, const glm::ivec3& p) {
            return level.GetBlockState(p.x, p.y, p.z);
        }
        void SetAt(ILevelWrite& level, const glm::ivec3& p, BlockState state, uint32_t flags) {
            level.SetBlock(p.x, p.y, p.z, state, flags);
        }

        // LevelHeightAccessor.isInsideBuildHeight(y).
        bool IsInsideBuildHeight(int y) { return y >= World::MIN_Y && y <= World::MAX_Y; }

        // #minecraft:flowers, resolved over every block once.
        bool IsFlower(BlockID id) {
            static std::once_flag once;
            static std::array<bool, BlockRegistry::Size> bits{};
            std::call_once(once, [] {
                for (size_t i = 0; i < BlockRegistry::Size; ++i) {
                    const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                    bits[i] = !slug.empty() &&
                              DataTags::HasTag(DataTags::Registry::Block, slug, "minecraft:flowers");
                }
            });
            const size_t i = static_cast<size_t>(id);
            return i < bits.size() && bits[i];
        }

        // TreeGrower.hasFlowers: any #flowers in pos + (-2,-1,-2) .. (2,1,2).
        bool HasFlowers(const IBlockAccess& level, const glm::ivec3& pos) {
            for (int x = -2; x <= 2; ++x) {
                for (int y = -1; y <= 1; ++y) {
                    for (int z = -2; z <= 2; ++z) {
                        if (IsFlower(level.GetBlock(pos.x + x, pos.y + y, pos.z + z))) return true;
                    }
                }
            }
            return false;
        }

        // TreeGrower.getSurroundingBlockStates / isTwoByTwoSapling /
        // findTwoByTwoSaplingPos: the four candidate squares in the order
        // (0,0) (0,-1) (-1,0) (-1,-1), each read as (dx,dz) (dx+1,dz)
        // (dx,dz+1) (dx+1,dz+1), every cell the same block as the sapling.
        struct TwoByTwo {
            int dx = 0;
            int dz = 0;
            std::array<std::pair<BlockState, glm::ivec3>, 4> cells{};
        };

        std::optional<TwoByTwo> FindTwoByTwoSaplingPos(const IBlockAccess& level, BlockState state,
                                                      const glm::ivec3& pos) {
            const BlockID block = state.Block();
            for (int dx = 0; dx >= -1; --dx) {
                for (int dz = 0; dz >= -1; --dz) {
                    TwoByTwo candidate;
                    candidate.dx = dx;
                    candidate.dz = dz;
                    const glm::ivec3 offsets[4] = {
                        {dx, 0, dz}, {dx + 1, 0, dz}, {dx, 0, dz + 1}, {dx + 1, 0, dz + 1},
                    };
                    bool all = true;
                    for (int i = 0; i < 4; ++i) {
                        const glm::ivec3 p = pos + offsets[i];
                        candidate.cells[i] = {StateAt(level, p), p};
                        if (!candidate.cells[i].first.Is(block)) all = false;
                    }
                    if (all) return candidate;
                }
            }
            return std::nullopt;
        }

        // TreeGrower.removeSapling: the cell becomes the fluid it held.
        void RemoveSapling(ILevelWrite& level, const glm::ivec3& pos) {
            SetAt(level, pos, FluidLegacyBlock(GetFluidState(level, pos)), kFlags818);
        }

        // TreeGrower.resetSaplings (flag 260, no UPDATE_CLIENTS). MC's
        // ChunkHolder collects the changed cell and sends whatever it holds
        // when the tick's changes flush — the sapling again, after the flag
        // 818 removal went out. This engine's accumulator records the state
        // at write time instead, so the cell is re-announced explicitly
        // (ServerLevel.sendBlockUpdated) to leave the client where MC's is.
        void ResetSaplings(ILevelWrite& level, const std::pair<BlockState, glm::ivec3>* cells, int count) {
            for (int i = 0; i < count; ++i) {
                SetAt(level, cells[i].second, cells[i].first, kFlags260);
                LiveFeatures::SendBlockUpdated(level, cells[i].second);
            }
        }

        // TreeGrower.growTree (26.3).
        bool GrowTreeModern(const TreeGrowerDef& g, ILevelWrite& level, const glm::ivec3& pos,
                            BlockState state, JavaRandom& random) {
            if (const auto mega = Pick(g.megaTrees, random)) {
                if (LiveFeatures::HasFeature(*mega)) {
                    if (const auto square = FindTwoByTwoSaplingPos(level, state, pos)) {
                        for (const auto& cell : square->cells) RemoveSapling(level, cell.second);
                        if (LiveFeatures::Place(level, *mega, pos + glm::ivec3(square->dx, 0, square->dz),
                                                random)) {
                            return true;
                        }
                        ResetSaplings(level, square->cells.data(), 4);
                        return false;
                    }
                }
            }

            const bool hasFlowers = HasFlowers(level, pos);
            const auto key = (hasFlowers && !g.flowerTrees.Empty()) ? Pick(g.flowerTrees, random)
                                                                   : Pick(g.trees, random);
            if (!key || !LiveFeatures::HasFeature(*key)) return false;
            RemoveSapling(level, pos);
            if (LiveFeatures::Place(level, *key, pos, random)) return true;
            const std::pair<BlockState, glm::ivec3> self{state, pos};
            ResetSaplings(level, &self, 1);
            return false;
        }

        // 1.21.1 TreeGrower.getConfiguredMegaFeature.
        std::string_view LegacyMegaFeature(const TreeGrowerDef& g, JavaRandom& random) {
            if (!g.secondaryMegaTree.empty() && random.NextFloat() < g.secondaryChance) {
                return g.secondaryMegaTree;
            }
            return g.megaTree;
        }

        // 1.21.1 TreeGrower.getConfiguredFeature — the nextFloat is drawn
        // even at secondaryChance 0.
        std::string_view LegacyFeature(const TreeGrowerDef& g, JavaRandom& random, bool hasFlowers) {
            if (random.NextFloat() < g.secondaryChance) {
                if (hasFlowers && !g.secondaryFlowers.empty()) return g.secondaryFlowers;
                if (!g.secondaryTree.empty()) return g.secondaryTree;
            }
            return (hasFlowers && !g.flowers.empty()) ? g.flowers : g.tree;
        }

        // 1.21.1 TreeGrower.growTree (the mods' saplings).
        bool GrowTreeLegacy(const TreeGrowerDef& g, ILevelWrite& level, const glm::ivec3& pos,
                            BlockState state, JavaRandom& random) {
            const std::string_view mega = LegacyMegaFeature(g, random);
            if (!mega.empty() && LiveFeatures::HasFeature(mega)) {
                if (const auto square = FindTwoByTwoSaplingPos(level, state, pos)) {
                    const BlockState air{};
                    for (const auto& cell : square->cells) SetAt(level, cell.second, air, kFlags4);
                    if (LiveFeatures::Place(level, mega, pos + glm::ivec3(square->dx, 0, square->dz),
                                            random)) {
                        return true;
                    }
                    // 1.21.1 puts the grown sapling's own state back in all four.
                    for (const auto& cell : square->cells) SetAt(level, cell.second, state, kFlags4);
                    return false;
                }
            }

            const std::string_view key = LegacyFeature(g, random, HasFlowers(level, pos));
            if (key.empty() || !LiveFeatures::HasFeature(key)) return false;
            const BlockState empty = FluidLegacyBlock(GetFluidState(level, pos));
            SetAt(level, pos, empty, kFlags4);
            if (LiveFeatures::Place(level, key, pos, random)) {
                // The flag-4 removal told nobody; a tree that did not build
                // over its own sapling cell tells the watchers now.
                if (StateAt(level, pos) == empty) LiveFeatures::SendBlockUpdated(level, pos);
                return true;
            }
            SetAt(level, pos, state, kFlags4);
            return false;
        }

        bool GrowTree(const TreeGrowerDef& g, ILevelWrite& level, const glm::ivec3& pos,
                      BlockState state, JavaRandom& random) {
            if (level.IsClientSide()) return false;
            return g.kind == GrowerKind::Modern ? GrowTreeModern(g, level, pos, state, random)
                                                : GrowTreeLegacy(g, level, pos, state, random);
        }

        // TreeGrower.canGrow (26.3): false only for a grower whose chosen
        // tree list is empty while it has mega trees and no 2x2 is planted —
        // dark oak and pale oak alone. Vanilla draws the two picks from the
        // level random on the way; a server level does the same here so the
        // level stream advances as vanilla's does.
        bool CanGrow(const TreeGrowerDef& g, const IBlockAccess& level, const glm::ivec3& pos,
                     BlockState state) {
            if (g.kind == GrowerKind::Legacy) return true;
            const bool hasFlowers = HasFlowers(level, pos);
            const WeightedList& treeList = (hasFlowers && !g.flowerTrees.Empty()) ? g.flowerTrees : g.trees;
            if (const auto* writable = dynamic_cast<const ILevelWrite*>(&level)) {
                if (JavaRandom* levelRandom = const_cast<ILevelWrite*>(writable)->Random()) {
                    (void)Pick(treeList, *levelRandom);
                    (void)Pick(g.megaTrees, *levelRandom);
                }
            }
            if (treeList.Empty() && !g.megaTrees.Empty()) {
                return FindTwoByTwoSaplingPos(level, state, pos).has_value();
            }
            return true;
        }

        // ── Light ───────────────────────────────────────────────────────────

        // MC Level.getMaxLocalRawBrightness(pos): max(block, sky - skyDarken).
        int MaxLocalRawBrightness(const ILevelWrite& level, const glm::ivec3& pos) {
            const World* world = dynamic_cast<const World*>(&level);
            const int darken = world ? world->GetSkyDarken() : 0;
            return level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z, darken);
        }

        // ── SaplingBlock ────────────────────────────────────────────────────

        bool IsLegacy(BlockID id) {
            const TreeGrowerDef* g = GrowerFor(id);
            return g && g->kind == GrowerKind::Legacy;
        }

        // SaplingBlock.advanceTree: stage 0 → state.cycle(STAGE) (flag 260;
        // 4 on the 1.21.1 mods), stage 1 → the grower.
        void AdvanceTree(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                         JavaRandom& random) {
            const TreeGrowerDef* g = GrowerFor(state.Block());
            if (!g) return;
            if (state.GetIndex(PropertyId::STAGE) == 0) {
                SetAt(level, pos, state.SetIndex(PropertyId::STAGE, 1),
                      IsLegacy(state.Block()) ? kFlags4 : kFlags260);
                return;
            }
            GrowTree(*g, level, pos, state, random);
        }

        bool AlwaysTicking(BlockState /*state*/) { return true; }

        // SaplingBlock.randomTick: BRIGHTNESS_FOR_SAPLING_GROWTH (9) above,
        // then TICK_CHANCE_FOR_SAPLING_GROWTH (1 in 7).
        void SaplingRandomTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                               JavaRandom& random) {
            if (MaxLocalRawBrightness(level, pos + glm::ivec3(0, 1, 0)) >= 9 && random.NextInt(7) == 0) {
                AdvanceTree(level, pos, state, random);
            }
        }

        // The Hush's whisperwood: SaplingBlock.randomTick WITHOUT the light
        // gate. The Hush is a fixed midnight lit only by the whisperwood's own
        // lantern leaves (docs/the-hush.md), so a sapling that waited for
        // brightness 9 would never grow at home. The 1-in-7 pacing is kept.
        void WhisperwoodRandomTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                   JavaRandom& random) {
            if (random.NextInt(7) == 0) AdvanceTree(level, pos, state, random);
        }

        // SaplingBlock.isValidBonemealTarget (26.3): canGrow, and the
        // grower's minimum height above the sapling inside the build height.
        // 1.21.1's (the mods) answers true.
        bool SaplingIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                          BlockState state) {
            const TreeGrowerDef* g = GrowerFor(state.Block());
            if (!g) return false;
            if (g->kind == GrowerKind::Legacy) return true;
            if (!CanGrow(*g, level, pos, state)) return false;
            const int heightOffset = g->minimumHeight >= 0 ? g->minimumHeight : 0;
            return IsInsideBuildHeight(pos.y + heightOffset);
        }

        // SaplingBlock.isBonemealSuccess: level.getRandom().nextFloat() < 0.45
        // (the bone meal item hands server-only growers the level random).
        bool SaplingIsBonemealSuccess(ILevelWrite& level, const glm::ivec3& /*pos*/,
                                      BlockState /*state*/, JavaRandom& random) {
            JavaRandom* levelRandom = level.Random();
            JavaRandom& rng = levelRandom ? *levelRandom : random;
            return static_cast<double>(rng.NextFloat()) < 0.45;
        }

        // SaplingBlock.performBonemeal → advanceTree.
        void SaplingPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                    JavaRandom& random) {
            AdvanceTree(level, pos, state, random);
        }

        // ── MangrovePropaguleBlock ──────────────────────────────────────────

        bool IsHanging(BlockState state) { return state.GetName(PropertyId::HANGING) == "true"; }
        int  PropaguleAge(BlockState state) { return state.GetIndex(PropertyId::AGE_4); }
        bool IsFullyGrown(BlockState state) { return PropaguleAge(state) == 4; }   // MAX_AGE

        // state.cycle(AGE) below MAX_AGE, flag 2.
        void GrowHangingPropagule(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            SetAt(level, pos, state.SetIndex(PropertyId::AGE_4, PropaguleAge(state) + 1), kFlags2);
        }

        // MangrovePropaguleBlock.randomTick: a standing propagule advances
        // 1 in 7 with NO light gate; a hanging one only ages.
        void PropaguleRandomTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                 JavaRandom& random) {
            if (!IsHanging(state)) {
                if (random.NextInt(7) == 0) AdvanceTree(level, pos, state, random);
            } else if (!IsFullyGrown(state)) {
                GrowHangingPropagule(level, pos, state);
            }
        }

        // MangrovePropaguleBlock.isValidBonemealTarget: !hanging || !fullyGrown.
        bool PropaguleIsValidBonemealTarget(const IBlockAccess& /*level*/, const glm::ivec3& /*pos*/,
                                            BlockState state) {
            return !IsHanging(state) || !IsFullyGrown(state);
        }

        // isBonemealSuccess: a hanging one always (while growing), a standing
        // one SaplingBlock's 45 %.
        bool PropaguleIsBonemealSuccess(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                        JavaRandom& random) {
            if (IsHanging(state)) return !IsFullyGrown(state);
            return SaplingIsBonemealSuccess(level, pos, state, random);
        }

        void PropagulePerformBonemeal(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                      JavaRandom& random) {
            if (IsHanging(state) && !IsFullyGrown(state)) {
                GrowHangingPropagule(level, pos, state);
            } else {
                AdvanceTree(level, pos, state, random);
            }
        }

        // ── AzaleaBlock ─────────────────────────────────────────────────────

        // isValidBonemealTarget: TreeGrower.AZALEA's minimum height + 2 above
        // inside the build height, and no fluid directly above.
        bool AzaleaIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockState /*state*/) {
            const int minHeight = kAzalea.minimumHeight >= 0 ? kAzalea.minimumHeight : 0;
            return IsInsideBuildHeight(pos.y + minHeight + 2) &&
                   GetFluidState(level, pos + glm::ivec3(0, 1, 0)).IsEmpty();
        }

        // performBonemeal: TreeGrower.AZALEA.growTree — no stage.
        void AzaleaPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                   JavaRandom& random) {
            GrowTree(kAzalea, level, pos, state, random);
        }

        // ── NetherFungusBlock ───────────────────────────────────────────────

        BlockID FungusRequiredBlock(BlockID fungus) {
            return fungus == BlockID::CrimsonFungus ? BlockID::CrimsonNylium : BlockID::WarpedNylium;
        }
        std::string_view FungusFeature(BlockID fungus) {
            return fungus == BlockID::CrimsonFungus ? "minecraft:crimson_fungus_planted"
                                                    : "minecraft:warped_fungus_planted";
        }

        // isValidBonemealTarget: the required nylium below, and the cell above
        // inside the build height.
        bool FungusIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockState state) {
            return level.GetBlock(pos.x, pos.y - 1, pos.z) == FungusRequiredBlock(state.Block()) &&
                   IsInsideBuildHeight(pos.y + 1);
        }

        // isBonemealSuccess: BONEMEAL_SUCCESS_PROBABILITY 0.4 on the random
        // handed in (the level's, from BoneMealItem.growCrop).
        bool FungusIsBonemealSuccess(ILevelWrite& /*level*/, const glm::ivec3& /*pos*/,
                                     BlockState /*state*/, JavaRandom& random) {
            return static_cast<double>(random.NextFloat()) < 0.4;
        }

        // performBonemeal: the planted huge fungus, placed at the fungus (the
        // feature clears its own origin).
        void FungusPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                   JavaRandom& random) {
            LiveFeatures::Place(level, FungusFeature(state.Block()), pos, random);
        }

    } // namespace

    void RegisterTreeGrowth(std::array<Block, BlockRegistry::Size>& blocks) {
        auto at = [&](BlockID id) -> Block& { return blocks[static_cast<size_t>(id)]; };

        // Growth is a random SHAPE the client cannot predict: every one of
        // these grows on the server only, from the level's random.
        auto wireBonemeal = [&](BlockID id, BlockIsValidBonemealTargetFn valid,
                                BlockIsBonemealSuccessFn success, BlockPerformBonemealFn perform) {
            Block& b = at(id);
            b.isValidBonemealTarget = valid;
            b.isBonemealSuccess     = success;
            b.performBonemeal       = perform;
            b.bonemealServerOnly    = true;
        };

        // SaplingBlock: the vanilla saplings and the mods' (TF and the Aether
        // register plain SaplingBlocks on their own growers).
        for (BlockID id : {BlockID::OakSapling, BlockID::SpruceSapling, BlockID::BirchSapling,
                           BlockID::JungleSapling, BlockID::AcaciaSapling, BlockID::CherrySapling,
                           BlockID::DarkOakSapling, BlockID::PaleOakSapling, BlockID::PoplarSapling,
                           BlockID::TwilightOakSapling, BlockID::CanopySapling,
                           BlockID::TfMangroveSapling, BlockID::DarkwoodSapling,
                           BlockID::SkyrootSapling, BlockID::GoldenOakSapling}) {
            Block& b = at(id);
            b.isRandomlyTicking = &AlwaysTicking;
            b.randomTick        = &SaplingRandomTick;
            wireBonemeal(id, &SaplingIsValidBonemealTarget, &SaplingIsBonemealSuccess,
                         &SaplingPerformBonemeal);
        }

        // The Hush's whisperwood: SaplingBlock without the light gate.
        {
            Block& b = at(BlockID::WhisperwoodSapling);
            b.isRandomlyTicking = &AlwaysTicking;
            b.randomTick        = &WhisperwoodRandomTick;
            wireBonemeal(BlockID::WhisperwoodSapling, &SaplingIsValidBonemealTarget,
                         &SaplingIsBonemealSuccess, &SaplingPerformBonemeal);
        }

        // MangrovePropaguleBlock.
        {
            Block& b = at(BlockID::MangrovePropagule);
            b.isRandomlyTicking = &AlwaysTicking;
            b.randomTick        = &PropaguleRandomTick;
            wireBonemeal(BlockID::MangrovePropagule, &PropaguleIsValidBonemealTarget,
                         &PropaguleIsBonemealSuccess, &PropagulePerformBonemeal);
        }

        // AzaleaBlock (azalea + flowering azalea): bone meal only, no ticks.
        for (BlockID id : {BlockID::Azalea, BlockID::FloweringAzalea}) {
            wireBonemeal(id, &AzaleaIsValidBonemealTarget, &SaplingIsBonemealSuccess,
                         &AzaleaPerformBonemeal);
        }

        // NetherFungusBlock: bone meal only, no ticks.
        for (BlockID id : {BlockID::CrimsonFungus, BlockID::WarpedFungus}) {
            wireBonemeal(id, &FungusIsValidBonemealTarget, &FungusIsBonemealSuccess,
                         &FungusPerformBonemeal);
        }
    }

} // namespace Game
