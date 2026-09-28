// File: src/common/world/block/SculkSpreader.cpp
#include "SculkSpreader.hpp"

#include "BlockPlacement.hpp"       // IsFaceSturdyAt
#include "BlockRegistry.hpp"
#include "MultifaceBlock.hpp"
#include "GeneratedBlockStates.hpp"
#include "RedstoneStateUtil.hpp"    // Relative, WithBool, BoolOf

#include "common/core/JavaRandom.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/sound/SoundType.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>

namespace Game {

    namespace {

        // setBlockAndUpdate (UPDATE_ALL) / setBlock(…, 2).
        constexpr uint32_t kUpdateAll = 3;
        constexpr uint32_t kUpdateClients = 2;

        // MC Direction.values() — the order every "for each direction" here
        // walks, and the ordinal MultifaceBlock.pack keys on.
        constexpr std::array<Direction, 6> kDirections = {
            Direction::Down, Direction::Up, Direction::North,
            Direction::South, Direction::West, Direction::East,
        };

        constexpr int Ordinal(Direction d) { return static_cast<int>(d); }

        // ── Block tags ──────────────────────────────────────────────────────
        enum TagBit : uint8_t {
            kReplaceable         = 1 << 0,   // #minecraft:sculk_replaceable
            kReplaceableWorldGen = 1 << 1,   // #minecraft:sculk_replaceable_world_gen
            kGrowthInhibitor     = 1 << 2,   // #minecraft:sculk_growth_inhibitors
            kFire                = 1 << 3,   // #minecraft:fire
        };

        const std::vector<uint8_t>& TagTable() {
            static const std::vector<uint8_t> table = [] {
                std::vector<uint8_t> t(BlockRegistry::Size, 0);
                for (size_t i = 0; i < t.size(); ++i) {
                    const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                    if (slug.empty()) continue;
                    for (const std::string& tag : DataTags::TagsFor(DataTags::Registry::Block, slug)) {
                        if (tag == "#minecraft:sculk_replaceable")                t[i] |= kReplaceable;
                        else if (tag == "#minecraft:sculk_replaceable_world_gen") t[i] |= kReplaceableWorldGen;
                        else if (tag == "#minecraft:sculk_growth_inhibitors")     t[i] |= kGrowthInhibitor;
                        else if (tag == "#minecraft:fire")                        t[i] |= kFire;
                    }
                }
                return t;
            }();
            return table;
        }
        bool HasTag(BlockID block, uint8_t bit) {
            const auto& t = TagTable();
            const size_t i = static_cast<size_t>(block);
            return i < t.size() && (t[i] & bit) != 0;
        }

        BlockState StateAt(const ILevelWrite& level, const glm::ivec3& p) {
            return level.GetBlockState(p.x, p.y, p.z);
        }

        bool IsAir(BlockState s) { return s.Block() == BlockID::Air; }

        // MC BlockState.canBeReplaced() — Properties.replaceable (air is).
        bool CanBeReplaced(BlockState s) {
            return IsAir(s) || BlockRegistry::Get(s.Block()).replaceable;
        }

        // MC Util.shuffle(list, random) — the Fisher-Yates walk vanilla uses
        // (swap i-1 with nextInt(i), from the top down).
        template <typename T, size_t N>
        void Shuffle(std::array<T, N>& list, JavaRandom& random) {
            for (int i = static_cast<int>(N); i > 1; --i) {
                const int swapTo = random.NextInt(i);
                std::swap(list[static_cast<size_t>(i - 1)], list[static_cast<size_t>(swapTo)]);
            }
        }

        // MC Direction.allShuffled(random).
        std::array<Direction, 6> AllShuffled(JavaRandom& random) {
            std::array<Direction, 6> dirs = kDirections;
            Shuffle(dirs, random);
            return dirs;
        }

        // MC ChargeCursor.NON_CORNER_NEIGHBOURS: BlockPos.betweenClosedStream
        // (-1,-1,-1)..(1,1,1) — x fastest, then y, then z — keeping the
        // offsets with a zero coordinate, minus the origin. 18 of them.
        std::array<glm::ivec3, 18> MakeNonCornerNeighbours() {
            std::array<glm::ivec3, 18> out{};
            size_t n = 0;
            for (int z = -1; z <= 1; ++z) {
                for (int y = -1; y <= 1; ++y) {
                    for (int x = -1; x <= 1; ++x) {
                        if ((x == 0 || y == 0 || z == 0) && !(x == 0 && y == 0 && z == 0)) {
                            out[n++] = glm::ivec3(x, y, z);
                        }
                    }
                }
            }
            return out;
        }
        const std::array<glm::ivec3, 18> kNonCornerNeighbours = MakeNonCornerNeighbours();

        // ── MultifaceSpreader over the sculk vein ───────────────────────────
        //
        // MC SculkVeinBlock builds two: veinSpreader (DEFAULT_SPREAD_ORDER)
        // and sameSpaceSpreader (SAME_POSITION only).
        enum class SpreadType : uint8_t { SamePosition, SamePlane, WrapAround };
        constexpr SpreadType kDefaultSpreadOrder[] = {
            SpreadType::SamePosition, SpreadType::SamePlane, SpreadType::WrapAround,
        };
        constexpr SpreadType kSamePositionOnly[] = { SpreadType::SamePosition };

        struct SpreadPos {
            glm::ivec3 pos;
            Direction  face;
        };

        SpreadPos GetSpreadPos(SpreadType type, const glm::ivec3& pos, Direction spreadDirection,
                               Direction fromFace) {
            switch (type) {
                case SpreadType::SamePosition: return {pos, spreadDirection};
                case SpreadType::SamePlane:    return {Relative(pos, spreadDirection), fromFace};
                case SpreadType::WrapAround:
                default:
                    return {Relative(Relative(pos, spreadDirection), fromFace), Opposite(spreadDirection)};
            }
        }

        // MultifaceBlock.hasFace — getValueOrElse(face property, false).
        bool HasFace(BlockState state, Direction face) { return MultifaceFaceOf(state, face); }
        bool HasAnyFace(BlockState state) {
            for (Direction d : kDirections) if (HasFace(state, d)) return true;
            return false;
        }

        // MultifaceBlock.canAttachTo(level, pos, direction).
        bool CanAttachTo(const ILevelWrite& level, const glm::ivec3& pos, Direction direction) {
            return MultifaceCanAttachTo(level, pos, direction);
        }

        // MultifaceBlock.isValidStateForPlacement for the sculk vein.
        bool IsValidStateForPlacement(const ILevelWrite& level, BlockState oldState, const glm::ivec3& placementPos,
                                      Direction placementDirection) {
            if (oldState.Block() == BlockID::SculkVein && HasFace(oldState, placementDirection)) return false;
            return CanAttachTo(level, placementPos, placementDirection);
        }

        // MultifaceBlock.getStateForPlacement(oldState, level, pos, direction).
        std::optional<BlockState> VeinStateForPlacement(const ILevelWrite& level, BlockState oldState,
                                                        const glm::ivec3& pos, Direction direction) {
            if (!IsValidStateForPlacement(level, oldState, pos, direction)) return std::nullopt;
            BlockState newState;
            if (oldState.Block() == BlockID::SculkVein) {
                newState = oldState;
            } else if (FluidStateOf(oldState).IsSourceOf(FluidType::Water)) {
                newState = WithBool(BlockStates::Default(BlockID::SculkVein), PropertyId::WATERLOGGED, true);
            } else {
                newState = BlockStates::Default(BlockID::SculkVein);
            }
            return MultifaceStateWithFace(newState, direction, true);
        }

        // SculkVeinSpreaderConfig.stateCanBeReplaced.
        bool StateCanBeReplaced(const ILevelWrite& level, const glm::ivec3& sourcePos, const glm::ivec3& placementPos,
                                Direction placementDirection, BlockState existingState) {
            const BlockID against = StateAt(level, Relative(placementPos, placementDirection)).Block();
            if (against == BlockID::Sculk || against == BlockID::SculkCatalyst || against == BlockID::MovingPiston) {
                return false;
            }
            const int manhattan = std::abs(sourcePos.x - placementPos.x) + std::abs(sourcePos.y - placementPos.y) +
                                  std::abs(sourcePos.z - placementPos.z);
            if (manhattan == 2) {
                const glm::ivec3 neighbourPos = Relative(sourcePos, Opposite(placementDirection));
                if (IsFaceSturdyAt(level, neighbourPos, placementDirection)) return false;
            }
            const FluidState fluid = FluidStateOf(existingState);
            // is(Fluids.WATER): the SOURCE fluid — flowing water refuses.
            if (!fluid.IsEmpty() && !fluid.IsSourceOf(FluidType::Water)) return false;
            if (HasTag(existingState.Block(), kFire)) return false;
            if (CanBeReplaced(existingState)) return true;
            // DefaultSpreaderConfig.stateCanBeReplaced.
            return IsAir(existingState) || existingState.Block() == BlockID::SculkVein ||
                   (existingState.Block() == BlockID::Water && fluid.IsSource());
        }

        // DefaultSpreaderConfig.canSpreadInto.
        bool CanSpreadInto(const ILevelWrite& level, const glm::ivec3& sourcePos, const SpreadPos& spreadPos) {
            const BlockState existing = StateAt(level, spreadPos.pos);
            return StateCanBeReplaced(level, sourcePos, spreadPos.pos, spreadPos.face, existing) &&
                   IsValidStateForPlacement(level, existing, spreadPos.pos, spreadPos.face);
        }

        // SculkVeinSpreaderConfig.isOtherBlockValidAsSource: anything but a vein.
        bool IsOtherBlockValidAsSource(BlockState state) { return state.Block() != BlockID::SculkVein; }
        bool CanSpreadFrom(BlockState state, Direction face) {
            return IsOtherBlockValidAsSource(state) || HasFace(state, face);
        }

        template <size_t N>
        std::optional<SpreadPos> GetSpreadFromFaceTowardDirection(const ILevelWrite& level, BlockState state,
                                                                  const glm::ivec3& pos, Direction startingFace,
                                                                  Direction spreadDirection,
                                                                  const SpreadType (&types)[N]) {
            if (AxisOf(spreadDirection) == AxisOf(startingFace)) return std::nullopt;
            if (!IsOtherBlockValidAsSource(state) &&
                (!HasFace(state, startingFace) || HasFace(state, spreadDirection))) {
                return std::nullopt;
            }
            for (SpreadType type : types) {
                const SpreadPos sp = GetSpreadPos(type, pos, spreadDirection, startingFace);
                if (CanSpreadInto(level, pos, sp)) return sp;
            }
            return std::nullopt;
        }

        // SpreadConfig.placeBlock → setBlock(pos, state, 2). `postProcess`
        // (the chunk's post-processing mark) only matters in worldgen, which
        // this engine's library runs itself.
        bool SpreadToFace(ILevelWrite& level, const SpreadPos& sp, bool /*postProcess*/) {
            const BlockState oldState = StateAt(level, sp.pos);
            const std::optional<BlockState> spreadState = VeinStateForPlacement(level, oldState, sp.pos, sp.face);
            if (!spreadState) return false;
            return level.SetBlock(sp.pos.x, sp.pos.y, sp.pos.z, *spreadState, kUpdateClients);
        }

        // MultifaceSpreader.spreadAll.
        template <size_t N>
        long SpreadAll(ILevelWrite& level, BlockState state, const glm::ivec3& pos, bool postProcess,
                       const SpreadType (&types)[N]) {
            long total = 0;
            for (Direction face : kDirections) {
                if (!CanSpreadFrom(state, face)) continue;
                // spreadFromFaceTowardAllDirections.
                for (Direction spreadDirection : kDirections) {
                    const std::optional<SpreadPos> sp =
                        GetSpreadFromFaceTowardDirection(level, state, pos, face, spreadDirection, types);
                    if (sp && SpreadToFace(level, *sp, postProcess)) ++total;
                }
            }
            return total;
        }

        // ── SculkVeinBlock ──────────────────────────────────────────────────

        // MC SculkVeinBlock.regrow.
        bool VeinRegrow(ILevelWrite& level, const glm::ivec3& pos, BlockState existing, uint8_t faces) {
            bool hasAtLeastOneFace = false;
            BlockState newState = BlockStates::Default(BlockID::SculkVein);
            for (Direction face : kDirections) {
                if ((faces & (1u << Ordinal(face))) == 0) continue;
                if (CanAttachTo(level, pos, face)) {
                    newState = MultifaceStateWithFace(newState, face, true);
                    hasAtLeastOneFace = true;
                }
            }
            if (!hasAtLeastOneFace) return false;
            if (!FluidStateOf(existing).IsEmpty()) newState = WithBool(newState, PropertyId::WATERLOGGED, true);
            level.SetBlock(pos.x, pos.y, pos.z, newState, kUpdateAll);
            return true;
        }

        // MC SculkVeinBlock.onDischarged.
        void VeinOnDischarged(ILevelWrite& level, BlockState state, const glm::ivec3& pos) {
            if (state.Block() != BlockID::SculkVein) return;
            for (Direction dir : kDirections) {
                if (HasFace(state, dir) && StateAt(level, Relative(pos, dir)).Block() == BlockID::Sculk) {
                    state = MultifaceStateWithFace(state, dir, false);
                }
            }
            if (!HasAnyFace(state)) {
                state = GetFluidState(level, pos).IsEmpty() ? BlockState{} : BlockStates::Default(BlockID::Water);
            }
            level.SetBlock(pos.x, pos.y, pos.z, state, kUpdateAll);
        }

        // MC SculkVeinBlock.attemptPlaceSculk.
        bool VeinAttemptPlaceSculk(const SculkSpreader& spreader, ILevelWrite& level, const glm::ivec3& pos,
                                   JavaRandom& random) {
            const BlockState state = StateAt(level, pos);
            for (Direction support : AllShuffled(random)) {
                if (!HasFace(state, support)) continue;
                const glm::ivec3 supportPos = Relative(pos, support);
                const BlockState supportState = StateAt(level, supportPos);
                if (!spreader.IsReplaceable(supportState.Block())) continue;
                const BlockState defaultSculk = BlockStates::Default(BlockID::Sculk);
                level.SetBlock(supportPos.x, supportPos.y, supportPos.z, defaultSculk, kUpdateAll);
                Sculk::PushEntitiesUp(level, supportState, supportPos);
                level.PlaySound(SoundExcept(nullptr), supportPos, SoundEvents::SCULK_BLOCK_SPREAD,
                                SoundSource::Blocks, 1.0f, 1.0f);
                SpreadAll(level, defaultSculk, supportPos, spreader.IsWorldGeneration(), kDefaultSpreadOrder);
                const Direction skip = Opposite(support);
                for (Direction veinBlocks : kDirections) {
                    if (veinBlocks == skip) continue;
                    const glm::ivec3 veinPos = Relative(supportPos, veinBlocks);
                    const BlockState possibleVein = StateAt(level, veinPos);
                    if (possibleVein.Block() == BlockID::SculkVein) VeinOnDischarged(level, possibleVein, veinPos);
                }
                return true;
            }
            return false;
        }

        // ── SculkBlock ──────────────────────────────────────────────────────

        // MC SculkBlock.canPlaceGrowth: open (air, or water source) above,
        // and at most two #sculk_growth_inhibitors in the 9x3x9 around.
        bool CanPlaceGrowth(const ILevelWrite& level, const glm::ivec3& pos) {
            const BlockState above = StateAt(level, Relative(pos, Direction::Up));
            const bool open = IsAir(above) ||
                              (above.Block() == BlockID::Water && FluidStateOf(above).IsSourceOf(FluidType::Water));
            if (!open) return false;
            int matched = 0;
            for (int x = pos.x - 4; x <= pos.x + 4; ++x) {
                for (int y = pos.y; y <= pos.y + 2; ++y) {
                    for (int z = pos.z - 4; z <= pos.z + 4; ++z) {
                        if (HasTag(level.GetBlock(x, y, z), kGrowthInhibitor) && ++matched > 2) return false;
                    }
                }
            }
            return true;
        }

        // MC SculkBlock.getRandomGrowthState.
        BlockState RandomGrowthState(const ILevelWrite& level, const glm::ivec3& pos, JavaRandom& random,
                                     bool isWorldGen) {
            BlockState state;
            if (random.NextInt(SculkSpreader::kShriekerPlacementRate) == 0) {
                state = WithBool(BlockStates::Default(BlockID::SculkShrieker), PropertyId::CAN_SUMMON, isWorldGen);
            } else {
                state = BlockStates::Default(BlockID::SculkSensor);
            }
            if (state.HasProperty(PropertyId::WATERLOGGED) && !GetFluidState(level, pos).IsEmpty()) {
                state = WithBool(state, PropertyId::WATERLOGGED, true);
            }
            return state;
        }

        double DistSqr(const glm::ivec3& a, const glm::ivec3& b) {
            const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        }

        // MC SculkBlock.getDecayPenalty.
        int DecayPenalty(const SculkSpreader& spreader, const glm::ivec3& pos, const glm::ivec3& originPos, int charge) {
            const int noGrowthRadius = spreader.NoGrowthRadius();
            const float d = static_cast<float>(std::sqrt(DistSqr(pos, originPos))) - static_cast<float>(noGrowthRadius);
            const float outerDistanceSquared = d * d;
            const int reach = SculkSpreader::kMaxGrowthRateRadius - noGrowthRadius;
            const int maxReachSquared = reach * reach;
            const float distanceFactor = std::min(1.0f, outerDistanceSquared / static_cast<float>(maxReachSquared));
            return std::max(1, static_cast<int>(static_cast<float>(charge) * distanceFactor * 0.5f));
        }

        // ── SculkBehaviour dispatch ─────────────────────────────────────────
        //
        // `instanceof SculkBehaviour`: the sculk block and the vein; every
        // other block spends a charge through SculkBehaviour.DEFAULT.
        enum class Behaviour : uint8_t { Default, Sculk, Vein };
        Behaviour BehaviourOf(BlockState state) {
            switch (state.Block()) {
                case BlockID::Sculk:     return Behaviour::Sculk;
                case BlockID::SculkVein: return Behaviour::Vein;
                default:                 return Behaviour::Default;
            }
        }
        bool IsSculkBehaviour(BlockState state) { return BehaviourOf(state) != Behaviour::Default; }

        bool AttemptSpreadVein(Behaviour b, ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                               const std::optional<uint8_t>& facings, bool postProcess) {
            if (b == Behaviour::Default) {
                if (!facings) {
                    // sameSpaceSpreader.spreadAll(level.getBlockState(pos), …).
                    return SpreadAll(level, StateAt(level, pos), pos, postProcess, kSamePositionOnly) > 0;
                }
                if (*facings != 0) {
                    if (!IsAir(state) && !FluidStateOf(state).IsSourceOf(FluidType::Water)) return false;
                    return VeinRegrow(level, pos, state, *facings);
                }
            }
            // The interface default: veinSpreader.spreadAll(state, …).
            return SpreadAll(level, state, pos, postProcess, kDefaultSpreadOrder) > 0;
        }

        bool CanChangeBlockStateOnSpread(Behaviour b) { return b != Behaviour::Sculk; }
        int UpdateDecayDelay(Behaviour b, int age) { return b == Behaviour::Default ? std::max(age - 1, 0) : 1; }
        int SculkSpreadDelay(Behaviour /*b*/) { return 1; }

        void OnDischarged(Behaviour b, ILevelWrite& level, BlockState state, const glm::ivec3& pos) {
            if (b == Behaviour::Vein) VeinOnDischarged(level, state, pos);
        }

        int AttemptUseCharge(Behaviour b, SculkSpreader::ChargeCursor& cursor, ILevelWrite& level,
                             const glm::ivec3& originPos, JavaRandom& random, const SculkSpreader& spreader,
                             bool spreadVeins) {
            switch (b) {
                case Behaviour::Default:
                    return cursor.decayDelay > 0 ? cursor.charge : 0;

                case Behaviour::Vein:
                    if (spreadVeins && VeinAttemptPlaceSculk(spreader, level, cursor.pos, random)) {
                        return cursor.charge - 1;
                    }
                    return random.NextInt(spreader.ChargeDecayRate()) == 0
                               ? static_cast<int>(std::floor(static_cast<float>(cursor.charge) * 0.5f))
                               : cursor.charge;

                case Behaviour::Sculk: {
                    const int charge = cursor.charge;
                    if (charge == 0 || random.NextInt(spreader.ChargeDecayRate()) != 0) return charge;
                    const glm::ivec3 chargePos = cursor.pos;
                    const double r = static_cast<double>(spreader.NoGrowthRadius());
                    const bool isCloseToCatalyst = DistSqr(chargePos, originPos) < r * r;
                    if (!isCloseToCatalyst && CanPlaceGrowth(level, chargePos)) {
                        const int xpPerGrowthSpawn = spreader.GrowthSpawnCost();
                        if (random.NextInt(xpPerGrowthSpawn) < charge) {
                            const glm::ivec3 growthPlacement = Relative(chargePos, Direction::Up);
                            const BlockState growthState =
                                RandomGrowthState(level, growthPlacement, random, spreader.IsWorldGeneration());
                            level.SetBlock(growthPlacement.x, growthPlacement.y, growthPlacement.z, growthState,
                                           kUpdateAll);
                            level.PlaySound(SoundExcept(nullptr), chargePos, SoundTypeOf(growthState).GetPlaceSound(),
                                            SoundSource::Blocks, 1.0f, 1.0f);
                        }
                        return std::max(0, charge - xpPerGrowthSpawn);
                    }
                    if (random.NextInt(spreader.AdditionalDecayRate()) != 0) return charge;
                    return charge - (isCloseToCatalyst ? 1 : DecayPenalty(spreader, chargePos, originPos, charge));
                }
            }
            return cursor.charge;
        }

        // ── ChargeCursor movement ───────────────────────────────────────────

        bool IsUnobstructed(const ILevelWrite& level, const glm::ivec3& from, Direction direction) {
            const glm::ivec3 testPos = Relative(from, direction);
            return !IsFaceSturdyAt(level, testPos, Opposite(direction));
        }

        // MC ChargeCursor.isMovementUnobstructed.
        bool IsMovementUnobstructed(const ILevelWrite& level, const glm::ivec3& from, const glm::ivec3& to) {
            const glm::ivec3 delta = to - from;
            if (std::abs(delta.x) + std::abs(delta.y) + std::abs(delta.z) == 1) return true;
            const Direction dx = delta.x < 0 ? Direction::West : Direction::East;
            const Direction dy = delta.y < 0 ? Direction::Down : Direction::Up;
            const Direction dz = delta.z < 0 ? Direction::North : Direction::South;
            if (delta.x == 0) return IsUnobstructed(level, from, dy) || IsUnobstructed(level, from, dz);
            if (delta.y == 0) return IsUnobstructed(level, from, dx) || IsUnobstructed(level, from, dz);
            return IsUnobstructed(level, from, dx) || IsUnobstructed(level, from, dy);
        }

        // MC ChargeCursor.canMoveToPos.
        bool CanMoveToPos(const glm::ivec3& origin, const glm::ivec3& target, const SculkSpreader& spreader) {
            if (!spreader.IsWorldGeneration()) return true;
            const int dx = origin.x - target.x;
            const int dz = origin.z - target.z;
            return dx * dx + dz * dz <= 144;
        }

        // MC ChargeCursor.getValidMovementPos.
        std::optional<glm::ivec3> GetValidMovementPos(const ILevelWrite& level, const glm::ivec3& pos,
                                                      JavaRandom& random, const glm::ivec3& originPos,
                                                      const SculkSpreader& spreader) {
            glm::ivec3 sculkPosition = pos;
            std::array<glm::ivec3, 18> offsets = kNonCornerNeighbours;
            Shuffle(offsets, random);
            for (const glm::ivec3& offset : offsets) {
                const glm::ivec3 neighbour = pos + offset;
                if (!CanMoveToPos(originPos, neighbour, spreader)) continue;
                const BlockState transferee = StateAt(level, neighbour);
                if (IsSculkBehaviour(transferee) && IsMovementUnobstructed(level, pos, neighbour)) {
                    sculkPosition = neighbour;
                    if (Sculk::HasSubstrateAccess(level, transferee, neighbour)) break;
                }
            }
            if (sculkPosition == pos) return std::nullopt;
            return sculkPosition;
        }

        // MC ChargeCursor.shouldUpdate.
        bool ShouldUpdate(const SculkSpreader::ChargeCursor& cursor, const World& level, const glm::ivec3& originPos,
                          bool isWorldGen) {
            if (cursor.charge <= 0) return false;
            if (isWorldGen) return true;
            return level.ShouldTickBlocksAt(originPos);
        }

        // MC ChargeCursor.update.
        void UpdateCursor(SculkSpreader::ChargeCursor& cursor, World& level, const glm::ivec3& originPos,
                          JavaRandom& random, const SculkSpreader& spreader, bool spreadVeins) {
            if (!ShouldUpdate(cursor, level, originPos, spreader.IsWorldGeneration())) return;
            if (cursor.updateDelay > 0) {
                --cursor.updateDelay;
                return;
            }
            BlockState currentState = StateAt(level, cursor.pos);
            Behaviour behaviour = BehaviourOf(currentState);
            if (spreadVeins && AttemptSpreadVein(behaviour, level, cursor.pos, currentState, cursor.facings,
                                                 spreader.IsWorldGeneration())) {
                if (CanChangeBlockStateOnSpread(behaviour)) {
                    currentState = StateAt(level, cursor.pos);
                    behaviour = BehaviourOf(currentState);
                }
                level.PlaySound(SoundExcept(nullptr), cursor.pos, SoundEvents::SCULK_BLOCK_SPREAD,
                                SoundSource::Blocks, 1.0f, 1.0f);
            }
            cursor.charge = AttemptUseCharge(behaviour, cursor, level, originPos, random, spreader, spreadVeins);
            if (cursor.charge <= 0) {
                OnDischarged(behaviour, level, currentState, cursor.pos);
                return;
            }
            if (const std::optional<glm::ivec3> transferPos =
                    GetValidMovementPos(level, cursor.pos, random, originPos, spreader)) {
                OnDischarged(behaviour, level, currentState, cursor.pos);
                cursor.pos = *transferPos;
                currentState = StateAt(level, *transferPos);
            } else if (spreader.IsWorldGeneration()) {
                OnDischarged(behaviour, level, currentState, cursor.pos);
                cursor.charge = 0;
                return;
            }
            // MultifaceBlock.availableFaces: the empty set for a non-multiface
            // sculk block, the vein's faces for a vein.
            if (IsSculkBehaviour(currentState)) {
                cursor.facings = currentState.Block() == BlockID::SculkVein ? Sculk::PackFaces(currentState)
                                                                            : uint8_t{0};
            }
            // MC reads these off the behaviour of the block the step STARTED
            // on (the local is not refreshed after the move).
            cursor.decayDelay = UpdateDecayDelay(behaviour, cursor.decayDelay);
            cursor.updateDelay = SculkSpreadDelay(behaviour);
        }

    } // namespace

    // ── SculkSpreader ───────────────────────────────────────────────────────

    SculkSpreader SculkSpreader::CreateLevelSpreader() { return SculkSpreader(false, 10, 4, 10, 5); }
    SculkSpreader SculkSpreader::CreateWorldGenSpreader() { return SculkSpreader(true, 50, 1, 5, 10); }

    bool SculkSpreader::IsReplaceable(BlockID block) const {
        return HasTag(block, m_isWorldGeneration ? kReplaceableWorldGen : kReplaceable);
    }

    void SculkSpreader::Load(const std::vector<ChargeCursor>& cursors) {
        m_cursors.clear();
        const size_t n = std::min(cursors.size(), static_cast<size_t>(kMaxCursors));
        for (size_t i = 0; i < n; ++i) AddCursor(cursors[i]);
    }

    void SculkSpreader::AddCursors(const glm::ivec3& startPos, int charge) {
        while (charge > 0) {
            const int currentCharge = std::min(charge, kMaxCharge);
            AddCursor(ChargeCursor(startPos, currentCharge));
            charge -= currentCharge;
        }
    }

    void SculkSpreader::UpdateCursors(World& level, const glm::ivec3& originPos, JavaRandom& random,
                                      bool spreadVeins) {
        if (m_cursors.empty()) return;

        struct PosLess {
            bool operator()(const glm::ivec3& a, const glm::ivec3& b) const {
                if (a.x != b.x) return a.x < b.x;
                if (a.y != b.y) return a.y < b.y;
                return a.z < b.z;
            }
        };
        std::vector<ChargeCursor> processed;
        processed.reserve(m_cursors.size());
        std::map<glm::ivec3, size_t, PosLess> mergeable;   // pos -> index into processed
        std::map<glm::ivec3, int, PosLess> chargeMap;

        // A copy: the step writes blocks, and nothing here may re-enter the
        // list being walked.
        std::vector<ChargeCursor> cursors = m_cursors;
        for (ChargeCursor& cursor : cursors) {
            // isPosUnreasonable: dropped without a trace.
            const glm::ivec3 d = glm::abs(cursor.pos - originPos);
            if (std::max(d.x, std::max(d.y, d.z)) > kMaxCursorDistance) continue;

            UpdateCursor(cursor, level, originPos, random, *this, spreadVeins);
            if (cursor.charge <= 0) {
                level.PlayLevelEvent(SoundExcept(nullptr), LevelEvent::PARTICLES_SCULK_CHARGE, cursor.pos, 0);
                continue;
            }
            chargeMap[cursor.pos] += cursor.charge;
            auto existing = mergeable.find(cursor.pos);
            if (existing == mergeable.end()) {
                mergeable[cursor.pos] = processed.size();
                processed.push_back(cursor);
            } else if (!m_isWorldGeneration && cursor.charge + processed[existing->second].charge <= kMaxCharge) {
                // ChargeCursor.mergeWith: the existing cursor absorbs this one.
                ChargeCursor& into = processed[existing->second];
                into.charge += cursor.charge;
                into.updateDelay = std::min(into.updateDelay, cursor.updateDelay);
            } else {
                const bool weaker = cursor.charge < processed[existing->second].charge;
                processed.push_back(cursor);
                if (weaker) existing->second = processed.size() - 1;
            }
        }

        // The charge's glow: one 3006 per occupied cell, sized by its charge
        // and drawn on the faces its cursor clings to.
        for (const auto& [pos, charge] : chargeMap) {
            auto it = mergeable.find(pos);
            if (it == mergeable.end()) continue;
            const std::optional<uint8_t>& faces = processed[it->second].facings;
            if (charge > 0 && faces) {
                const int numParticles = static_cast<int>(std::log1p(static_cast<double>(charge)) / 2.299999952316284) + 1;
                const int data = (numParticles << 6) + static_cast<int>(*faces);
                level.PlayLevelEvent(SoundExcept(nullptr), LevelEvent::PARTICLES_SCULK_CHARGE, pos, data);
            }
        }

        m_cursors = std::move(processed);
    }

    // ── Sculk helpers ───────────────────────────────────────────────────────

    namespace Sculk {

        uint8_t PackFaces(BlockState state) {
            uint8_t code = 0;
            for (Direction d : kDirections) {
                if (HasFace(state, d)) code = static_cast<uint8_t>(code | (1u << Ordinal(d)));
            }
            return code;
        }

        bool IsSculkReplaceable(BlockID block) { return HasTag(block, kReplaceable); }

        bool HasSubstrateAccess(const ILevelWrite& level, BlockState state, const glm::ivec3& pos) {
            if (state.Block() != BlockID::SculkVein) return false;
            for (Direction direction : kDirections) {
                if (HasFace(state, direction) &&
                    HasTag(StateAt(level, Relative(pos, direction)).Block(), kReplaceable)) {
                    return true;
                }
            }
            return false;
        }

        void PushEntitiesUp(ILevelWrite& level, BlockState oldState, const glm::ivec3& pos) {
            // Shapes.joinUnoptimized(old collision, full cube, ONLY_SECOND):
            // the slice of the cell above the old block's collision top. A
            // full cube had none — nothing to lift.
            const BlockRegistry::BlockShapeSet oldShape = BlockRegistry::GetBlockCollisionShapeSet(oldState);
            if (BlockRegistry::HasCollision(oldState.Block()) && oldShape.IsFullCube()) return;
            double oldTop = 0.0;
            if (BlockRegistry::HasCollision(oldState.Block())) {
                for (const BlockRegistry::BlockShape& b : oldShape) oldTop = std::max(oldTop, static_cast<double>(b.max.y));
            }
            const double sliceMin = pos.y + oldTop;
            const double top = pos.y + 1.0;
            EntityLevel* entities = level.Entities();
            if (!entities) return;

            AABB box;
            box.min = glm::vec3(static_cast<float>(pos.x), static_cast<float>(sliceMin), static_cast<float>(pos.z));
            box.max = glm::vec3(static_cast<float>(pos.x + 1), static_cast<float>(top), static_cast<float>(pos.z + 1));
            std::vector<Entity*> found;
            entities->GetEntitiesInBox(box, nullptr, found);
            for (Entity* e : found) {
                if (!e || e->IsRemoved() || e->IsPlayer()) continue;
                // teleportRelative(0, 1 + offset, 0): feet onto the new top.
                if (e->position.y < top) e->position.y = top;
            }
            std::vector<LivingEntity*> players;
            entities->GetPlayers(players);
            for (LivingEntity* p : players) {
                if (!p) continue;
                const AABBd b = p->GetAABBd();
                if (b.max.x <= pos.x || b.min.x >= pos.x + 1 || b.max.z <= pos.z || b.min.z >= pos.z + 1) continue;
                if (b.min.y >= top || b.max.y <= sliceMin) continue;
                entities->TeleportPlayer(*p, glm::dvec3(p->position.x, top, p->position.z));
            }
        }

    } // namespace Sculk

} // namespace Game
