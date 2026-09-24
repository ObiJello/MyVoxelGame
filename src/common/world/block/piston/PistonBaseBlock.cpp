// File: src/common/world/block/piston/PistonBaseBlock.cpp
#include "common/world/block/piston/PistonBaseBlock.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/RedstoneSignal.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/block/entity/PistonMovingBlockEntity.hpp"
#include "common/world/block/piston/PistonStructureResolver.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/NeighborUpdater.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace Game {

    namespace {

        constexpr int kTriggerExtend   = 0;
        constexpr int kTriggerContract = 1;
        constexpr int kTriggerDrop     = 2;

        // MC's write flags, by value, so each call reads like the decompile.
        // 276 and 324 carry no UPDATE_CLIENTS: the moving cells are never
        // sent as block updates. The client receives the block EVENT and runs
        // this same triggerEvent against its own level, exactly as vanilla.
        constexpr uint32_t kClients = World::UpdateFlags::UpdateClients;
        constexpr uint32_t F67  = 64 | 2 | 1;
        constexpr uint32_t F276 = 256 | 16 | 4;
        constexpr uint32_t F324 = 256 | 64 | 4;
        constexpr uint32_t F82  = 64 | 16 | 2;
        constexpr uint32_t F18  = 16 | 2;

        BlockState StateAt(const IBlockAccess& level, const glm::ivec3& p) {
            return level.GetBlockState(p.x, p.y, p.z);
        }
        void SetBlock(ILevelWrite& level, const glm::ivec3& pos, BlockState state, uint32_t flags) {
            level.SetBlock(pos.x, pos.y, pos.z, state, flags);
        }
        void RemoveBlock(ILevelWrite& level, const glm::ivec3& pos, bool movedByPiston) {
            if (auto* world = dynamic_cast<World*>(&level)) world->RemoveBlock(pos, movedByPiston);
            else level.SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);
        }

        bool IsSticky(BlockID id) { return id == BlockID::StickyPiston; }
        bool IsPistonBase(BlockID id) { return id == BlockID::Piston || id == BlockID::StickyPiston; }

        // PISTON_TYPE: normal, sticky.
        BlockState WithPistonType(BlockState s, bool sticky) {
            return s.SetIndex(PropertyId::PISTON_TYPE, sticky ? 1 : 0);
        }

        // MC MovingPistonBlock.newMovingBlockEntity + Level.setBlockEntity.
        void InstallMovingEntity(ILevelWrite& level, const glm::ivec3& pos, BlockState movedState,
                                 Direction direction, bool extending, bool isSourcePiston) {
            const auto* type = BlockEntityTypes::ForId(BlockEntityTypeIds::PISTON);
            if (!type) return;
            auto be = std::make_unique<PistonMovingBlockEntity>(type, pos, BlockID::MovingPiston);
            be->Init(movedState, direction, extending, isSourcePiston);
            level.SetBlockEntity(pos, std::move(be));
        }

        // MC PistonBaseBlock.getNeighborSignal: any side but the push face,
        // the cell below, or any side of the cell above (quasi-connectivity).
        bool GetNeighborSignal(const IBlockAccess& level, const glm::ivec3& pos, Direction pushDirection) {
            for (Direction direction : kAllDirections) {
                if (direction != pushDirection && HasSignal(level, Relative(pos, direction), direction)) return true;
            }
            if (HasSignal(level, pos, Direction::Down)) return true;
            const glm::ivec3 above = Above(pos);
            for (Direction direction : kAllDirections) {
                if (direction != Direction::Down && HasSignal(level, Relative(above, direction), direction)) return true;
            }
            return false;
        }

        // MC PistonBaseBlock.checkIfExtend.
        void CheckIfExtend(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            const Direction direction = FacingOf(state);
            const bool extend = GetNeighborSignal(level, pos, direction);
            if (extend && !BoolOf(state, PropertyId::EXTENDED)) {
                PistonStructureResolver resolver(level, pos, direction, true);
                if (resolver.Resolve()) {
                    level.BlockEvent(pos, state.Block(), kTriggerExtend, static_cast<int>(direction));
                } else {
                    // OBEY_PISTON_DEBUG: why a powered piston stayed put. The
                    // partial toPush list is what the resolver had gathered
                    // when it gave up — 12 entries means the push limit, and
                    // with a slime block that usually means it dragged the
                    // floor along (vanilla does the same).
                    static const bool kPistonDebug = std::getenv("OBEY_PISTON_DEBUG") != nullptr;
                    if (kPistonDebug) {
                        std::string gathered;
                        for (const glm::ivec3& p : resolver.GetToPush()) {
                            gathered += " " + BlockRegistry::Get(StateAt(level, p).Block()).registrySlug +
                                        "@" + std::to_string(p.x) + "," + std::to_string(p.y) + "," + std::to_string(p.z);
                        }
                        Log::Info("[PistonDbg] piston at (%d,%d,%d) facing %d cannot extend; gathered %zu:%s",
                                  pos.x, pos.y, pos.z, static_cast<int>(direction),
                                  resolver.GetToPush().size(), gathered.c_str());
                    }
                }
            } else if (!extend && BoolOf(state, PropertyId::EXTENDED)) {
                const glm::ivec3 pushedPos = Relative(pos, direction, 2);
                const BlockState pushedState = StateAt(level, pushedPos);
                int event = kTriggerContract;
                if (pushedState.Is(BlockID::MovingPiston) && FacingOf(pushedState) == direction) {
                    if (auto* pistonEntity = dynamic_cast<PistonMovingBlockEntity*>(level.GetBlockEntity(pushedPos))) {
                        if (pistonEntity->IsExtending() &&
                            (pistonEntity->GetProgress(0.0f) < 0.5f ||
                             level.GameTime() == pistonEntity->GetLastTicked() ||
                             level.IsHandlingTick())) {
                            event = kTriggerDrop;
                        }
                    }
                }
                level.BlockEvent(pos, state.Block(), event, static_cast<int>(direction));
            }
        }

        // MC PistonBaseBlock.moveBlocks.
        bool MoveBlocks(ILevelWrite& level, const glm::ivec3& pistonPos, Direction direction,
                        bool extending, bool sticky) {
            const glm::ivec3 armPos = Relative(pistonPos, direction);
            if (!extending && StateAt(level, armPos).Is(BlockID::PistonHead)) {
                SetBlock(level, armPos, BlockState{}, F276);
            }
            PistonStructureResolver resolver(level, pistonPos, direction, extending);
            if (!resolver.Resolve()) return false;

            // A HashMap<BlockPos, BlockState> in vanilla; only membership and
            // the (unordered) final iteration matter, and a vector keeps the
            // write order deterministic.
            std::vector<std::pair<glm::ivec3, BlockState>> deleteAfterMove;
            auto removeDelete = [&](const glm::ivec3& p) {
                for (size_t i = 0; i < deleteAfterMove.size(); ++i) {
                    if (deleteAfterMove[i].first == p) { deleteAfterMove.erase(deleteAfterMove.begin() + static_cast<long>(i)); return; }
                }
            };

            const std::vector<glm::ivec3>& toPush = resolver.GetToPush();
            std::vector<BlockState> toPushShapes;
            for (const glm::ivec3& pos : toPush) {
                const BlockState state = StateAt(level, pos);
                toPushShapes.push_back(state);
                deleteAfterMove.emplace_back(pos, state);
            }
            const std::vector<glm::ivec3>& toDestroy = resolver.GetToDestroy();
            std::vector<BlockState> toUpdate;
            toUpdate.reserve(toPush.size() + toDestroy.size());
            const Direction pushDirection = extending ? direction : Opposite(direction);

            for (int i = static_cast<int>(toDestroy.size()) - 1; i >= 0; --i) {
                const glm::ivec3 pos = toDestroy[static_cast<size_t>(i)];
                const BlockState blockState = StateAt(level, pos);
                DropBlockLoot(level, pos, blockState);
                SetBlock(level, pos, BlockState{}, F18);
                toUpdate.push_back(blockState);
            }

            for (int i = static_cast<int>(toPush.size()) - 1; i >= 0; --i) {
                glm::ivec3 pos = toPush[static_cast<size_t>(i)];
                const BlockState blockState = StateAt(level, pos);
                pos = Relative(pos, pushDirection);
                removeDelete(pos);
                const BlockState state = WithFacing(BlockStates::Default(BlockID::MovingPiston), direction);
                SetBlock(level, pos, state, F324);
                InstallMovingEntity(level, pos, toPushShapes[static_cast<size_t>(i)], direction, extending, false);
                toUpdate.push_back(blockState);
            }

            if (extending) {
                const BlockState headState = WithPistonType(
                    WithFacing(BlockStates::Default(BlockID::PistonHead), direction), sticky);
                const BlockState blockState = WithPistonType(
                    WithFacing(BlockStates::Default(BlockID::MovingPiston), direction), sticky);
                removeDelete(armPos);
                SetBlock(level, armPos, blockState, F324);
                InstallMovingEntity(level, armPos, headState, direction, true, true);
            }

            const BlockState air{};
            for (const auto& [pos, oldState] : deleteAfterMove) {
                SetBlock(level, pos, air, F82);
            }
            if (auto* world = dynamic_cast<World*>(&level)) {
                for (const auto& [pos, oldState] : deleteAfterMove) {
                    // oldState.updateIndirectNeighbourShapes(level, pos, 2);
                    // air.updateNeighbourShapes(level, pos, 2);
                    // air.updateIndirectNeighbourShapes(level, pos, 2);
                    if (const Block& def = BlockRegistry::Get(oldState.Block()); def.updateIndirectNeighbourShapes) {
                        def.updateIndirectNeighbourShapes(level, pos, oldState, 2, World::kUpdateLimit);
                    }
                    world->UpdateNeighbourShapes(air, pos, 2, World::kUpdateLimit);
                }
            }

            size_t updateIndex = 0;
            for (int i = static_cast<int>(toDestroy.size()) - 1; i >= 0; --i) {
                const BlockState state = toUpdate[updateIndex++];
                const glm::ivec3 pos = toDestroy[static_cast<size_t>(i)];
                if (const Block& def = BlockRegistry::Get(state.Block()); def.affectNeighborsAfterRemoval) {
                    def.affectNeighborsAfterRemoval(level, pos, state, false);
                }
                if (const Block& def = BlockRegistry::Get(state.Block()); def.updateIndirectNeighbourShapes) {
                    def.updateIndirectNeighbourShapes(level, pos, state, 2, World::kUpdateLimit);
                }
                level.UpdateNeighborsAt(pos, state.Block());
            }
            for (int i = static_cast<int>(toPush.size()) - 1; i >= 0; --i) {
                level.UpdateNeighborsAt(toPush[static_cast<size_t>(i)], toUpdate[updateIndex++].Block());
            }
            if (extending) level.UpdateNeighborsAt(armPos, BlockID::PistonHead);
            return true;
        }

        // ── PistonBaseBlock hooks ────────────────────────────────────────

        void PistonNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockID, bool) {
            if (!level.IsClientSide()) CheckIfExtend(level, pos, state);
        }

        void PistonOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockState oldState, bool) {
            if (oldState.Block() == state.Block()) return;
            if (!level.IsClientSide() && level.GetBlockEntity(pos) == nullptr) CheckIfExtend(level, pos, state);
        }

        bool PistonTriggerEvent(ILevelWrite& level, const glm::ivec3& pos, BlockState state, int b0, int b1) {
            const Direction direction = FacingOf(state);
            const BlockState extendedState = WithBool(state, PropertyId::EXTENDED, true);
            const bool sticky = IsSticky(state.Block());
            if (!level.IsClientSide()) {
                const bool extend = GetNeighborSignal(level, pos, direction);
                if (extend && (b0 == kTriggerContract || b0 == kTriggerDrop)) {
                    SetBlock(level, pos, extendedState, kClients);
                    return false;
                }
                if (!extend && b0 == kTriggerExtend) return false;
            }
            JavaRandom* random = level.Random();
            const float rf = random ? random->NextFloat() : 0.5f;
            if (b0 == kTriggerExtend) {
                if (!MoveBlocks(level, pos, direction, true, sticky)) return false;
                SetBlock(level, pos, extendedState, F67);
                // MC PistonBaseBlock.triggerEvent:169 — playSound(null, …).
                level.PlaySound(nullptr, pos, SoundEvents::PISTON_EXTEND, SoundSource::Blocks, 0.5f,
                                rf * 0.25f + 0.6f);
            } else if (b0 == kTriggerContract || b0 == kTriggerDrop) {
                if (auto* prev = dynamic_cast<PistonMovingBlockEntity*>(level.GetBlockEntity(Relative(pos, direction)))) {
                    prev->FinalTick(level);
                }
                const BlockState movingPistonState = WithPistonType(
                    WithFacing(BlockStates::Default(BlockID::MovingPiston), direction), sticky);
                SetBlock(level, pos, movingPistonState, F276);
                InstallMovingEntity(level, pos, WithFacing(BlockStates::Default(state.Block()),
                                                           static_cast<Direction>(b1 & 7)),
                                    direction, false, true);
                level.UpdateNeighborsAt(pos, BlockID::MovingPiston);
                if (auto* world = dynamic_cast<World*>(&level)) {
                    world->UpdateNeighbourShapes(movingPistonState, pos, 2, World::kUpdateLimit);
                }
                if (sticky) {
                    const glm::ivec3 twoPos = Relative(pos, direction, 2);
                    const BlockState movingState = StateAt(level, twoPos);
                    bool pistonPiece = false;
                    if (movingState.Is(BlockID::MovingPiston)) {
                        if (auto* entity = dynamic_cast<PistonMovingBlockEntity*>(level.GetBlockEntity(twoPos))) {
                            if (entity->GetDirection() == direction && entity->IsExtending()) {
                                entity->FinalTick(level);
                                pistonPiece = true;
                            }
                        }
                    }
                    if (!pistonPiece) {
                        const PushReaction reaction = BlockRegistry::Get(movingState.Block()).pushReaction;
                        if (b0 != kTriggerContract || movingState.Block() == BlockID::Air ||
                            !PistonIsPushable(movingState, level, twoPos, Opposite(direction), false, direction) ||
                            (reaction != PushReaction::PushPull && !IsPistonBase(movingState.Block()))) {
                            RemoveBlock(level, Relative(pos, direction), false);
                        } else {
                            MoveBlocks(level, pos, direction, false, sticky);
                        }
                    }
                } else {
                    RemoveBlock(level, Relative(pos, direction), false);
                }
                // MC PistonBaseBlock.triggerEvent:209.
                level.PlaySound(nullptr, pos, SoundEvents::PISTON_CONTRACT, SoundSource::Blocks, 0.5f,
                                rf * 0.15f + 0.6f);
            }
            return true;
        }

        // ── PistonHeadBlock hooks ────────────────────────────────────────

        bool IsFittingBase(BlockState armState, BlockState potentialBase) {
            const BlockID baseBlock = armState.GetIndex(PropertyId::PISTON_TYPE) == 0
                ? BlockID::Piston : BlockID::StickyPiston;
            return potentialBase.Is(baseBlock) && BoolOf(potentialBase, PropertyId::EXTENDED) &&
                   FacingOf(potentialBase) == FacingOf(armState);
        }

        void HeadAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool) {
            const glm::ivec3 basePos = Relative(pos, Opposite(FacingOf(state)));
            if (IsFittingBase(state, StateAt(level, basePos))) level.DestroyBlock(basePos, true);
        }

        bool HeadUpdateShape(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                             Direction toNeighbour, BlockID, BlockState& outState, ScheduledTickAccess*) {
            if (Opposite(toNeighbour) == FacingOf(state) && !PistonHeadCanSurvive(level, pos, state)) {
                outState = BlockState{};
                return true;
            }
            return false;
        }

        void HeadNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockID block, bool) {
            if (PistonHeadCanSurvive(level, pos, state)) {
                level.NeighborChanged(Relative(pos, Opposite(FacingOf(state))), block);
            }
        }

        // ── MovingPistonBlock hooks ──────────────────────────────────────

        UseResult MovingPistonUse(ILevelWrite* level, const glm::ivec3& pos, IUsePlayer*, const BlockHitResult&) {
            if (!level) return UseResult::Pass;
            if (!level->IsClientSide() && level->GetBlockEntity(pos) == nullptr) {
                RemoveBlock(*level, pos, false);
                return UseResult::Consume;
            }
            return UseResult::Pass;
        }

        // MC MovingPistonBlock.destroy: breaking the cell in transit also
        // retracts the extended base behind it.
        void MovingPistonAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool movedByPiston) {
            if (movedByPiston) return;
            const glm::ivec3 relative = Relative(pos, Opposite(FacingOf(state)));
            const BlockState blockState = StateAt(level, relative);
            if (IsPistonBase(blockState.Block()) && BoolOf(blockState, PropertyId::EXTENDED)) {
                RemoveBlock(level, relative, false);
            }
        }

    } // namespace

    bool PistonHeadCanSurvive(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
        const BlockState base = StateAt(level, Relative(pos, Opposite(FacingOf(state))));
        return IsFittingBase(state, base) ||
               (base.Is(BlockID::MovingPiston) && FacingOf(base) == FacingOf(state));
    }

    void PistonPlacedBy(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
        if (!level.IsClientSide() && IsPistonBase(state.Block())) CheckIfExtend(level, pos, state);
    }

    BlockState UpdateFromNeighbourShapes(ILevelWrite& level, BlockState state, const glm::ivec3& pos) {
        BlockState newState = state;
        for (Direction direction : CollectingNeighborUpdater::kUpdateShapeOrder) {
            const Block& def = BlockRegistry::Get(newState.Block());
            if (!def.updateShape) continue;
            const glm::ivec3 neighbourPos = Relative(pos, direction);
            BlockState out;
            if (def.updateShape(level, pos, newState, direction, level.GetBlock(neighbourPos.x, neighbourPos.y, neighbourPos.z),
                                out, level.Ticks())) {
                newState = out;
            }
        }
        return newState;
    }

    void RegisterPistonBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        for (BlockID id : {BlockID::Piston, BlockID::StickyPiston}) {
            Block& b = blocks[static_cast<size_t>(id)];
            b.neighborChanged = &PistonNeighborChanged;
            b.onPlace         = &PistonOnPlace;
            b.triggerEvent    = &PistonTriggerEvent;
        }
        {
            Block& head = blocks[static_cast<size_t>(BlockID::PistonHead)];
            head.affectNeighborsAfterRemoval = &HeadAfterRemoval;
            head.updateShape                 = &HeadUpdateShape;
            head.neighborChanged             = &HeadNeighborChanged;
        }
        {
            Block& moving = blocks[static_cast<size_t>(BlockID::MovingPiston)];
            moving.useWithoutItem              = &MovingPistonUse;
            moving.affectNeighborsAfterRemoval = &MovingPistonAfterRemoval;
        }
    }

} // namespace Game
