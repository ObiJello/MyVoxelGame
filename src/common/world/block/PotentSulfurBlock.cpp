// File: src/common/world/block/PotentSulfurBlock.cpp
//
// MC PotentSulfurBlock (26.3), method by method, plus the static geometry
// PotentSulfurBlockEntity keeps (see the header for why it lives here).
//
// DEVIATIONS:
//   * MC's CollisionContext.positionContext (the geyser tests pass one) only
//     changes the collision shape of blocks that ask about the entity or the
//     position — scaffolding, powder snow. The engine's collision shapes are
//     per state, so those two read as their default shape here.
//   * level.gameEvent(BLOCK_ACTIVATE) has nothing to reach: the engine has no
//     game-event (vibration) system.
#include "common/world/block/PotentSulfurBlock.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/sound/LevelSound.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/entity/PotentSulfurBlockEntity.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/BlockClip.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Game {

    namespace PotentSulfur {

        State StateOf(BlockState state) {
            const int index = state.GetIndex(PropertyId::POTENT_SULFUR_STATE);
            return index < 0 ? State::Dry : static_cast<State>(index);
        }

        BlockState WithState(BlockState state, State value) {
            return state.SetIndex(PropertyId::POTENT_SULFUR_STATE, static_cast<int>(value));
        }

        namespace {

            // `state.is(TagKey)` against the data pack's block tags.
            bool BlockHasTag(BlockState state, const char* tag) {
                const BlockID id = state.Block();
                if (id == BlockID::Air) return false;
                const Block& def = BlockRegistry::Get(id);
                if (def.registrySlug.empty()) return false;
                const std::vector<std::string>& tags =
                    DataTags::TagsFor(DataTags::Registry::Block, def.registrySlug);
                return std::binary_search(tags.begin(), tags.end(), std::string(tag));
            }

            // MC PotentSulfurBlock.isSourceIfFluid: no fluid, or a source.
            bool IsSourceIfFluid(BlockState below) {
                const FluidState fluid = FluidStateOf(below);
                return fluid.IsEmpty() || fluid.IsSource();
            }

            bool IsWaterSource(const IBlockAccess& level, const glm::ivec3& pos) {
                return GetFluidState(level, pos).IsSourceOf(FluidType::Water);
            }

            bool IsPassableState(BlockState state) {
                const BlockID id = state.Block();
                if (id == BlockID::Air || id == BlockID::Water) return true;
                return BlockRegistry::GetBlockCollisionShapeSet(state).count == 0;
            }

            glm::ivec3 Containing(const glm::dvec3& p) {
                return glm::ivec3(static_cast<int>(std::floor(p.x)),
                                  static_cast<int>(std::floor(p.y)),
                                  static_cast<int>(std::floor(p.z)));
            }

            glm::dvec3 CenterOf(const glm::ivec3& p) {
                return glm::dvec3(p.x + 0.5, p.y + 0.5, p.z + 0.5);
            }

        } // namespace

        BlockState ValidBlockState(BlockState state, const IBlockAccess& level,
                                   const glm::ivec3& pos, ILevelWrite* blockEntities) {
            if (!IsWaterSource(level, Above(pos))) {
                return WithState(state, State::Dry);
            }
            const glm::ivec3 belowPos = Below(pos);
            const BlockState below = level.GetBlockState(belowPos.x, belowPos.y, belowPos.z);
            if (BlockHasTag(below, "#minecraft:causes_continuous_geyser_eruptions") &&
                IsSourceIfFluid(below)) {
                return WithState(state, State::Continuous);
            }
            if (BlockHasTag(below, "#minecraft:causes_periodic_geyser_eruptions") &&
                IsSourceIfFluid(below)) {
                const State current = StateOf(state);
                const bool isGeyser = current == State::Erupting || current == State::Dormant;
                if (!isGeyser && blockEntities) {
                    if (auto* be = dynamic_cast<PotentSulfurBlockEntity*>(
                            blockEntities->GetBlockEntity(pos))) {
                        be->ResetCountdown();
                    }
                }
                return current == State::Erupting ? state : WithState(state, State::Dormant);
            }
            return WithState(state, State::Wet);
        }

        bool IsGeyserPassable(const IBlockAccess& level, const glm::ivec3& pos) {
            return IsPassableState(level.GetBlockState(pos.x, pos.y, pos.z));
        }

        std::optional<glm::ivec3> FindNoxiousGasSourceBlock(const IBlockAccess& level,
                                                            const glm::ivec3& origin) {
            const int maxY = origin.y + kAllowedWaterBlocksAbove + 1;
            glm::ivec3 pos = Above(origin);
            while (pos.y <= maxY) {
                const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
                const bool isWaterLogged = IsWaterSource(level, pos);
                if (isWaterLogged && (state.Is(BlockID::Water) || IsPassableState(state))) {
                    ++pos.y;
                    continue;
                }
                if (state.Is(BlockID::Air) || IsPassableState(state)) return pos;
                return std::nullopt;
            }
            return std::nullopt;
        }

        bool CanBeReachedByNoxiousGas(const IBlockAccess& level, const glm::ivec3& sourceBlock,
                                      const glm::dvec3& pos) {
            if (!IsGeyserPassable(level, Containing(pos))) return false;
            const glm::dvec3 d = pos - CenterOf(sourceBlock);
            if (glm::dot(d, d) > 9.0) return false;
            const glm::dvec3 belowSource = CenterOf(Below(sourceBlock));
            const glm::dvec3 belowPos(pos.x, pos.y - 1.0, pos.z);
            if (!IsWaterSource(level, Containing(belowPos))) return false;
            // haveLineOfSight: level.clip(COLLIDER, Fluid.NONE) misses.
            glm::ivec3 hit;
            return !ClipBlocksCollider(level, belowSource, belowPos, hit, /*includeWaterSource=*/false);
        }

        int GetUnobstructedBlockCount(const IBlockAccess& level, const glm::ivec3& pos,
                                      int waterBlocks) {
            const int geyserForceHeight = 6 * waterBlocks;
            for (int i = 0; i < geyserForceHeight; ++i) {
                if (!IsGeyserPassable(level, glm::ivec3(pos.x, pos.y + i, pos.z))) return i;
            }
            return geyserForceHeight;
        }

    } // namespace PotentSulfur

    namespace {

        using PotentSulfur::State;

        // MC PotentSulfurBlock.updateShape: any neighbour change re-derives
        // the state (validBlockState). The level MC hands it is the one whose
        // block entity a new geyser's countdown is reset on; here that is the
        // writable level behind the reader, when there is one.
        bool PotentSulfurUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                     BlockState state, Direction /*toNeighbour*/,
                                     BlockID /*neighbourId*/, BlockState& outState,
                                     ScheduledTickAccess* /*ticks*/) {
            auto* writable = const_cast<ILevelWrite*>(dynamic_cast<const ILevelWrite*>(&level));
            const BlockState next = PotentSulfur::ValidBlockState(state, level, pos, writable);
            if (next == state) return false;
            outState = next;
            return true;
        }

        // MC PotentSulfurBlock.onPlace: a state that starts an eruption
        // (every write into ERUPTING or CONTINUOUS, state-only ones included
        // — MC does not filter on the old state) sends block event 0, which
        // restarts both sides' eruption clock, and plays the start sound.
        void PotentSulfurOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState newState,
                                 BlockState /*oldState*/, bool /*movedByPiston*/) {
            if (level.IsClientSide()) return;
            const State s = PotentSulfur::StateOf(newState);
            if (s != State::Erupting && s != State::Continuous) return;
            level.BlockEvent(pos, BlockID::PotentSulfur, 0, 0);
            level.PlaySound(SoundExcept(nullptr), pos,
                            s == State::Continuous ? SoundEvents::GEYSER_CONTINUOUS_START
                                                   : SoundEvents::GEYSER_ERUPTION_START,
                            SoundSource::Blocks, 1.0f, 1.0f);
            // level.gameEvent(GameEvent.BLOCK_ACTIVATE, pos, Context.of(state)):
            // no game-event system (see the file header).
        }

        // MC PotentSulfurBlock.animateTick: under a water source, every state
        // but DRY bubbles — two SULFUR_BUBBLES a tick in the cell above
        // (addAlwaysVisibleParticle) — and hisses one tick in ten. The sound
        // is played at the cell's CORNER, as MC passes getX/getY/getZ.
        void PotentSulfurAnimateTick(EntityLevel& level, const glm::ivec3& pos, BlockState state,
                                     JavaRandom& random) {
            if (PotentSulfur::StateOf(state) == State::Dry) return;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            if (!GetFluidState(*blocks, Above(pos)).IsSourceOf(FluidType::Water)) return;
            for (int i = 0; i < 2; ++i) {
                const double x = static_cast<double>(pos.x) + static_cast<double>(random.NextFloat());
                const double y = static_cast<double>(pos.y + 1) + static_cast<double>(random.NextFloat());
                const double z = static_cast<double>(pos.z) + static_cast<double>(random.NextFloat());
                level.AddParticle(ParticleKind::SulfurBubbles, x, y, z, 0.0, 0.0, 0.0);
            }
            if (random.NextInt(10) == 0) {
                level.PlayLocalSound(glm::dvec3(pos), SoundEvents::NOXIOUS_GAS, SoundSource::Ambient,
                                     1.0f, 1.0f, false);
            }
        }

        // MC PotentSulfurBlock.triggerEvent: any block event stamps the block
        // entity's eruption clock with the level's game time (the plume and
        // its sound are phased from it) and reports handled, so the server
        // passes it on to the clients.
        bool PotentSulfurTriggerEvent(ILevelWrite& level, const glm::ivec3& pos,
                                      BlockState /*state*/, int /*b0*/, int /*b1*/) {
            if (auto* be = dynamic_cast<PotentSulfurBlockEntity*>(level.GetBlockEntity(pos))) {
                be->eruptionTick = level.GameTime();
            }
            return true;
        }

    } // namespace

    void RegisterPotentSulfurBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        Block& b = blocks[static_cast<size_t>(BlockID::PotentSulfur)];
        b.updateShape  = &PotentSulfurUpdateShape;
        b.onPlace      = &PotentSulfurOnPlace;
        b.animateTick  = &PotentSulfurAnimateTick;
        b.triggerEvent = &PotentSulfurTriggerEvent;
    }

} // namespace Game
