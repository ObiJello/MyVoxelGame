// File: src/common/world/block/RedstoneComponents.cpp
#include "common/world/block/RedstoneComponents.hpp"
#include "common/world/lighting/ChunkLight.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/sound/SoundType.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/decoration/ItemFrame.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/RedstonePlus.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/CrossCollision.hpp"
#include "common/world/block/FenceGate.hpp"
#include "common/world/block/RedstoneFamilies.hpp"
#include "common/world/block/RedstoneSignal.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/RedstoneWire.hpp"
#include "common/world/block/TntBlock.hpp"
#include "common/world/block/Walls.hpp"
#include "common/world/block/entity/ComparatorBlockEntity.hpp"
#include "common/world/block/entity/DaylightDetectorBlockEntity.hpp"
#include "common/world/block/piston/PistonBaseBlock.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <limits>
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>
#include <tuple>
#include <unordered_set>

namespace Game {

    namespace {

        constexpr float kPi = 3.1415927f;

        // ── Shared helpers ───────────────────────────────────────────────

        BlockState StateAt(const IBlockAccess& level, const glm::ivec3& p) {
            return level.GetBlockState(p.x, p.y, p.z);
        }

        // MC Level.setBlockAndUpdate — flag 3.
        void SetBlockAndUpdate(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            level.SetBlock(pos.x, pos.y, pos.z, state, World::UpdateFlags::All);
        }
        void SetBlockClients(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            level.SetBlock(pos.x, pos.y, pos.z, state, World::UpdateFlags::UpdateClients);
        }

        // MC Level.removeBlock(pos, movedByPiston).
        void RemoveBlock(ILevelWrite& level, const glm::ivec3& pos, bool movedByPiston) {
            if (auto* world = dynamic_cast<World*>(&level)) {
                world->RemoveBlock(pos, movedByPiston);
            } else {
                level.SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);
            }
        }

        void ScheduleTick(ILevelWrite& level, const glm::ivec3& pos, BlockID block, int delay,
                          TickPriority priority = TickPriority::Normal) {
            if (auto* ticks = level.Ticks()) ticks->ScheduleTick(pos, block, delay, priority);
        }
        bool HasScheduledTick(ILevelWrite& level, const glm::ivec3& pos, BlockID block) {
            auto* ticks = level.Ticks();
            return ticks && ticks->HasScheduledTick(pos, block);
        }

        float RandomFloat(ILevelWrite& level) {
            JavaRandom* r = level.Random();
            return r ? r->NextFloat() : 0.5f;
        }

        // MC BlockBehaviour.isFaceSturdy(level, pos, UP, SupportType.RIGID) —
        // the whole top square is solid.
        bool CanSupportRigidBlock(const IBlockAccess& level, const glm::ivec3& pos) {
            return IsFaceSturdyAt(level, pos, Direction::Up);
        }
        // MC Block.canSupportCenter(level, pos, UP): SupportType.CENTER — the
        // centre of the top face carries. Fence posts, walls and hoppers pass
        // here without a full top square; the engine has no support types, so
        // the families vanilla's CENTER test admits are listed.
        bool CanSupportCenter(const IBlockAccess& level, const glm::ivec3& pos) {
            if (IsFaceSturdyAt(level, pos, Direction::Up)) return true;
            const BlockID id = level.GetBlock(pos.x, pos.y, pos.z);
            return IsCrossCollisionBlock(id) || IsWallBlock(id) || id == BlockID::Hopper ||
                   IsFenceGateBlock(id);
        }

        // Entity counting for plates, buttons and tripwire. MC
        // getEntitiesOfClass(cls, box, NO_SPECTATORS && !isIgnoringBlockTriggers).
        struct EntityCount {
            int living = 0;   // LivingEntity (mobs, players)
            int other  = 0;   // every other Game::Entity (arrows, TNT, boats…)
            int items  = 0;   // dropped items
            int arrows = 0;   // AbstractArrow
            int all() const { return living + other + items; }
        };

        EntityCount CountEntitiesInBox(ILevelWrite& level, const AABBd& box) {
            EntityCount count;
            EntityLevel* entities = level.Entities();
            if (!entities) return count;

            std::vector<Entity*> found;
            entities->GetEntitiesInBox(AABB::FromMinMax(glm::vec3(box.min), glm::vec3(box.max)),
                                       nullptr, found);
            for (Entity* e : found) {
                if (!e || e->IsRemoved() || e->IsSpectator()) continue;
                if (dynamic_cast<LivingEntity*>(e)) ++count.living;
                else                                ++count.other;
                const EntityTypeId t = e->GetType();
                if (t == EntityTypeId::Arrow || t == EntityTypeId::Trident) ++count.arrows;
            }
            std::vector<EntityLevel::NearbyItemEntity> items;
            entities->GetItemEntitiesInBox(box, items);
            count.items = static_cast<int>(items.size());
            return count;
        }

        AABBd CellBox(const glm::ivec3& pos, double x0, double y0, double z0,
                      double x1, double y1, double z1) {
            return AABBd::FromMinMax(glm::dvec3(pos) + glm::dvec3(x0, y0, z0),
                                     glm::dvec3(pos) + glm::dvec3(x1, y1, z1));
        }

        // ── Redstone torch (RedstoneTorchBlock / RedstoneWallTorchBlock) ─

        struct Toggle {
            glm::ivec3 pos;
            int64_t    when;
        };
        // MC RECENT_TOGGLES, a WeakHashMap<Level, List<Toggle>>. Keyed on the
        // level so each dimension burns out independently.
        std::unordered_map<const ILevelWrite*, std::vector<Toggle>> s_recentToggles;

        constexpr int  kRecentToggleTimer = 60;
        constexpr int  kMaxRecentToggles  = 8;
        constexpr int  kRestartDelay      = 160;
        constexpr int  kToggleDelay       = 2;

        bool IsToggledTooFrequently(ILevelWrite& level, const glm::ivec3& pos, bool add) {
            std::vector<Toggle>& toggles = s_recentToggles[&level];
            if (add) toggles.push_back(Toggle{pos, level.GameTime()});
            int count = 0;
            for (const Toggle& t : toggles) {
                if (t.pos == pos && ++count >= kMaxRecentToggles) return true;
            }
            return false;
        }

        bool TorchIsWall(BlockState s) { return s.Is(BlockID::RedstoneWallTorch) || s.Is(BlockID::BlueRedstoneWallTorch); }
        bool TorchIsBlue(BlockState s) { return s.Is(BlockID::BlueRedstoneTorch) || s.Is(BlockID::BlueRedstoneWallTorch); }

        // notifyNeighbors: updateNeighborsAt on all six neighbours.
        void TorchNotifyNeighbors(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            for (Direction d : kAllDirections) level.UpdateNeighborsAt(Relative(pos, d), state.Block());
        }

        // hasNeighborSignal: the block the torch is attached to.
        bool TorchHasNeighborSignal(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            if (TorchIsWall(state)) {
                const Direction opposite = Opposite(HorizontalFacingOf(state));
                return HasSignal(level, Relative(pos, opposite), opposite);
            }
            return HasSignal(level, Below(pos), Direction::Down);
        }

        void TorchOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                          BlockState /*oldState*/, bool /*movedByPiston*/) {
            // No block-change guard in vanilla: a lit→unlit flip re-notifies.
            TorchNotifyNeighbors(level, pos, state);
        }

        void TorchAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                               bool movedByPiston) {
            if (!movedByPiston) TorchNotifyNeighbors(level, pos, state);
        }

        void TorchTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            const bool neighborSignal = TorchHasNeighborSignal(level, pos, state);
            std::vector<Toggle>& toggles = s_recentToggles[&level];
            while (!toggles.empty() && level.GameTime() - toggles.front().when > kRecentToggleTimer) {
                toggles.erase(toggles.begin());
            }
            // redstone_plus: no burn-out. The toggle list is not even fed,
            // so a fast clock cannot leave a backlog behind for vanilla.
            const bool canBurnOut = !RedstonePlus::Enabled();
            if (LitOf(state)) {
                if (neighborSignal) {
                    SetBlockAndUpdate(level, pos, WithLit(state, false));
                    if (canBurnOut && IsToggledTooFrequently(level, pos, true)) {
                        // MC levelEvent 1502: the burn-out smoke + fizz — its
                        // sound half (LevelEventHandler: 0.5, 2.6 ± 0.8).
                        PlayLevelEventSound(level, nullptr, LevelEvent::REDSTONE_TORCH_BURNOUT, pos, 0,
                                            level.Random());
                        ScheduleTick(level, pos, StateAt(level, pos).Block(), kRestartDelay);
                    }
                }
            } else if (!neighborSignal && (!canBurnOut || !IsToggledTooFrequently(level, pos, false))) {
                SetBlockAndUpdate(level, pos, WithLit(state, true));
            }
        }

        // ── redstone_plus: deferred re-checks ────────────────────────────
        //
        // With zero-delay gates a combinational network settles inside one
        // neighbour-update cascade, and the ORDER of that cascade is not
        // something two implementations agree on (this engine, the tick
        // sim, a real Minecraft with the same rule would all differ). A
        // repeater or red torch that re-checks its input mid-cascade can see
        // a transient — and a repeater that schedules on a transient turns
        // on regardless (RepeaterBlock: a scheduled repeater always turns
        // on, minimum pulse = delay). So under the rule the delayed
        // components do not decide during the cascade: they are listed, and
        // re-check once against the settled state when the tick's block
        // updates are done (World::ProcessBlockUpdates flushes). Their tick
        // lands at the same time it would have (same tick + delay); only the
        // glitch is gone. Blue torches and dust stay immediate.
        struct Deferred { std::vector<glm::ivec3> list; std::unordered_map<uint64_t, uint8_t> seen; };
        std::unordered_map<const ILevelWrite*, Deferred> s_deferred;
        uint64_t PosKey(const glm::ivec3& p);

        void Defer(ILevelWrite& level, const glm::ivec3& pos) {
            Deferred& d = s_deferred[&level];
            if (d.seen.emplace(PosKey(pos), 1).second) d.list.push_back(pos);
        }

        void TorchCheck(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            if (LitOf(state) == TorchHasNeighborSignal(level, pos, state) &&
                !HasScheduledTick(level, pos, state.Block())) {
                ScheduleTick(level, pos, state.Block(), kToggleDelay);
            }
        }

        void TorchNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                  BlockID, bool) {
            if (RedstonePlus::Enabled()) { Defer(level, pos); return; }
            TorchCheck(level, pos, state);
        }

        // ── The blue torch (redstone_plus) ───────────────────────────────
        //
        // A redstone torch with no delay: while the rule is on it flips the
        // moment its input changes, inside the same neighbour update, so a
        // chain of gates settles within the tick like combinational logic in
        // a real circuit. Feedback needs a delay to mean anything (a blue
        // torch feeding itself would flip forever inside one tick), so each
        // torch gets a budget of instant flips per game tick; past it the
        // torch falls back to the ordinary scheduled flip, which turns a loop
        // of blue torches into a tick-rate clock instead of a hang. With the
        // rule off a blue torch is a redstone torch.
        constexpr int kMaxInstantFlipsPerTick = 8;
        struct InstantFlips { int64_t tick = -1; std::unordered_map<uint64_t, int> count; };
        std::unordered_map<const ILevelWrite*, InstantFlips> s_instantFlips;

        uint64_t PosKey(const glm::ivec3& p) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(p.x)) << 38) ^
                   (static_cast<uint64_t>(static_cast<uint32_t>(p.y) & 0xFFFu) << 26) ^
                   static_cast<uint64_t>(static_cast<uint32_t>(p.z) & 0x3FFFFFFu);
        }

        void BlueTorchNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                      BlockID source, bool moved) {
            if (!RedstonePlus::Enabled()) { TorchNeighborChanged(level, pos, state, source, moved); return; }
            const bool lit = LitOf(state);
            if (lit != TorchHasNeighborSignal(level, pos, state)) return;      // already right
            InstantFlips& flips = s_instantFlips[&level];
            if (flips.tick != level.GameTime()) { flips.tick = level.GameTime(); flips.count.clear(); }
            int& n = flips.count[PosKey(pos)];
            if (n >= kMaxInstantFlipsPerTick) {
                if (!HasScheduledTick(level, pos, state.Block())) ScheduleTick(level, pos, state.Block(), kToggleDelay);
                return;
            }
            ++n;
            // setBlock with UPDATE_ALL: the neighbours (and, through the block
            // it powers, their neighbours) hear the new state now.
            SetBlockAndUpdate(level, pos, WithLit(state, !lit));
        }

        int TorchOwnSignal(BlockState state) { return LitOf(state) ? 15 : 0; }

        int TorchGetSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction direction) {
            if (TorchIsWall(state)) {
                return HorizontalFacingOf(state) != direction ? TorchOwnSignal(state) : 0;
            }
            return direction != Direction::Up ? TorchOwnSignal(state) : 0;
        }

        int TorchGetDirectSignal(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                                 Direction direction) {
            return direction == Direction::Down ? TorchGetSignal(level, pos, state, direction) : 0;
        }

        // WallTorchBlock.canSurvive(level, pos, facing): the block behind
        // must present a sturdy face toward the torch.
        bool WallTorchCanSurvive(const IBlockAccess& level, const glm::ivec3& pos, Direction facing) {
            const glm::ivec3 behind = Relative(pos, Opposite(facing));
            return IsFaceSturdyAt(level, behind, facing);
        }

        bool WallTorchUpdateShape(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                                  Direction toNeighbour, BlockID, BlockState& outState,
                                  ScheduledTickAccess*) {
            const Direction facing = HorizontalFacingOf(state);
            if (Opposite(toNeighbour) == facing && !WallTorchCanSurvive(level, pos, facing)) {
                outState = BlockState{};
                return true;
            }
            return false;
        }

        // ── DiodeBlock (repeater, comparator) ────────────────────────────

        bool DiodeCanSurviveOn(const IBlockAccess& level, const glm::ivec3& below) {
            return IsFaceSturdyAt(level, below, Direction::Up);
        }

        int DiodeGetDelay(BlockState state) {
            // RepeaterBlock: DELAY * 2 (DELAY index 0 == delay 1); ComparatorBlock: 2.
            return state.Is(BlockID::Repeater) ? (state.GetIndex(PropertyId::DELAY) + 1) * 2 : 2;
        }

        bool DiodeSideInputDiodesOnly(BlockState state) { return state.Is(BlockID::Repeater); }

        // DiodeBlock.getInputSignal — the rear input, with dust read directly.
        int DiodeGetInputSignal(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
            const Direction direction = HorizontalFacingOf(state);
            const glm::ivec3 targetPos = Relative(pos, direction);
            const int input = GetSignal(level, targetPos, direction);
            if (input >= 15) return input;
            const BlockState targetState = StateAt(level, targetPos);
            return std::max(input, targetState.Is(BlockID::RedstoneWire) ? PowerOf(targetState) : 0);
        }

        // DiodeBlock.getAlternateSignal — the two side inputs.
        int DiodeGetAlternateSignal(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
            const Direction direction = HorizontalFacingOf(state);
            const Direction clockWise = ClockWise(direction);
            const Direction counterClockWise = CounterClockWise(direction);
            const bool sideInputDiodesOnly = DiodeSideInputDiodesOnly(state);
            return std::max(
                GetControlInputSignal(level, Relative(pos, clockWise), clockWise, sideInputDiodesOnly),
                GetControlInputSignal(level, Relative(pos, counterClockWise), counterClockWise,
                                      sideInputDiodesOnly));
        }

        bool RepeaterIsLocked(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
            return DiodeGetAlternateSignal(level, pos, state) > 0;
        }
        bool DiodeIsLocked(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
            return state.Is(BlockID::Repeater) && RepeaterIsLocked(level, pos, state);
        }

        // ComparatorBlock.getItemFrame: the one item frame in the cell that
        // faces the comparator's way (hung on the far side of the conductor),
        // or null when there is none — or more than one.
        const ItemFrame* ComparatorItemFrame(ILevelWrite& level, Direction direction, const glm::ivec3& pos) {
            EntityLevel* entities = level.Entities();
            if (!entities) return nullptr;
            AABB box;
            box.min = glm::vec3(pos);
            box.max = glm::vec3(pos) + glm::vec3(1.0f);
            std::vector<Entity*> found;
            entities->GetEntitiesInBox(box, nullptr, found);
            const ItemFrame* frame = nullptr;
            int count = 0;
            for (const Entity* e : found) {
                const auto* f = dynamic_cast<const ItemFrame*>(e);
                if (!f || f->IsRemoved() || f->GetDirection() != direction) continue;
                if (!f->GetAABB().Intersects(box)) continue;
                frame = f;
                ++count;
            }
            return count == 1 ? frame : nullptr;
        }

        // ComparatorBlock.getInputSignal — DiodeBlock's, then a container's
        // analog reading, directly in front or, one conductor away, the
        // larger of a container's reading and an item frame's.
        int ComparatorGetInputSignal(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            int resultSignal = DiodeGetInputSignal(level, pos, state);
            const Direction direction = HorizontalFacingOf(state);
            glm::ivec3 targetPos = Relative(pos, direction);
            BlockState targetState = StateAt(level, targetPos);
            const Block& targetDef = BlockRegistry::Get(targetState.Block());
            if (targetDef.hasAnalogOutputSignal && targetDef.getAnalogOutputSignal) {
                resultSignal = targetDef.getAnalogOutputSignal(level, targetPos, targetState,
                                                               Opposite(direction));
            } else if (resultSignal < 15 && IsRedstoneConductor(level, targetPos, targetState)) {
                targetPos = Relative(targetPos, direction);
                targetState = StateAt(level, targetPos);
                const Block& farDef = BlockRegistry::Get(targetState.Block());
                const ItemFrame* frame = ComparatorItemFrame(level, direction, targetPos);
                int signal = frame ? frame->GetAnalogOutput() : std::numeric_limits<int>::min();
                if (farDef.hasAnalogOutputSignal && farDef.getAnalogOutputSignal) {
                    signal = std::max(signal, farDef.getAnalogOutputSignal(level, targetPos, targetState,
                                                                           Opposite(direction)));
                }
                if (signal != std::numeric_limits<int>::min()) resultSignal = signal;
            }
            return resultSignal;
        }

        int ComparatorOutputFromEntity(ILevelWrite& level, const glm::ivec3& pos) {
            if (auto* be = dynamic_cast<ComparatorBlockEntity*>(level.GetBlockEntity(pos))) {
                return be->GetOutputSignal();
            }
            return 0;
        }

        bool ComparatorIsSubtract(BlockState state) {
            return state.GetName(PropertyId::MODE_COMPARATOR) == "subtract";
        }

        int ComparatorCalculateOutputSignal(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            const int inputSignal = ComparatorGetInputSignal(level, pos, state);
            if (inputSignal == 0) return 0;
            const int alternateSignal = DiodeGetAlternateSignal(level, pos, state);
            if (alternateSignal > inputSignal) return 0;
            return ComparatorIsSubtract(state) ? inputSignal - alternateSignal : inputSignal;
        }

        bool DiodeShouldTurnOn(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            if (state.Is(BlockID::Comparator)) {
                const int input = ComparatorGetInputSignal(level, pos, state);
                if (input == 0) return false;
                const int sideInput = DiodeGetAlternateSignal(level, pos, state);
                if (input > sideInput) return true;
                return input == sideInput && !ComparatorIsSubtract(state);
            }
            return DiodeGetInputSignal(level, pos, state) > 0;
        }

        // DiodeBlock.getOutputSignal: 15 for a repeater, the entity's value
        // for a comparator. Read-only, so the comparator reaches its block
        // entity through a const_cast'd level — the entity is not written.
        int DiodeGetOutputSignal(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
            if (!state.Is(BlockID::Comparator)) return 15;
            if (auto* write = dynamic_cast<ILevelWrite*>(const_cast<IBlockAccess*>(&level))) {
                return ComparatorOutputFromEntity(*write, pos);
            }
            return 0;
        }

        int DiodeOwnSignal(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
            return PoweredOf(state) ? DiodeGetOutputSignal(level, pos, state) : 0;
        }

        int DiodeGetSignal(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                           Direction direction) {
            return HorizontalFacingOf(state) == direction ? DiodeOwnSignal(level, pos, state) : 0;
        }

        // DiodeBlock.shouldPrioritize: a diode feeding INTO this one's rear
        // gets its tick first.
        bool DiodeShouldPrioritize(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
            const Direction direction = Opposite(HorizontalFacingOf(state));
            const BlockState oppositeState = StateAt(level, Relative(pos, direction));
            return IsDiodeBlock(oppositeState.Block()) &&
                   HorizontalFacingOf(oppositeState) != direction;
        }

        // DiodeBlock.updateNeighborsInFront.
        void DiodeUpdateNeighborsInFront(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            const Direction direction = HorizontalFacingOf(state);
            const glm::ivec3 oppositePos = Relative(pos, Opposite(direction));
            level.NeighborChanged(oppositePos, state.Block());
            level.UpdateNeighborsAtExceptFromFacing(oppositePos, state.Block(), direction);
        }

        void ComparatorRefreshOutputState(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            const int outputValue = ComparatorCalculateOutputSignal(level, pos, state);
            int oldValue = 0;
            if (auto* be = dynamic_cast<ComparatorBlockEntity*>(level.GetBlockEntity(pos))) {
                oldValue = be->GetOutputSignal();
                be->SetOutputSignal(outputValue);
            }
            if (oldValue != outputValue || !ComparatorIsSubtract(state)) {
                const bool sourceOn = DiodeShouldTurnOn(level, pos, state);
                const bool isOn = PoweredOf(state);
                if (isOn && !sourceOn) {
                    SetBlockClients(level, pos, WithPowered(state, false));
                } else if (!isOn && sourceOn) {
                    SetBlockClients(level, pos, WithPowered(state, true));
                }
                DiodeUpdateNeighborsInFront(level, pos, state);
            }
        }

        // DiodeBlock.checkTickOnNeighbor, with ComparatorBlock's override.
        void DiodeCheckTickOnNeighbor(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            if (state.Is(BlockID::Comparator)) {
                if (HasScheduledTick(level, pos, state.Block())) return;
                const int outputValue = ComparatorCalculateOutputSignal(level, pos, state);
                const int oldValue = ComparatorOutputFromEntity(level, pos);
                if (outputValue != oldValue || PoweredOf(state) != DiodeShouldTurnOn(level, pos, state)) {
                    const TickPriority priority = DiodeShouldPrioritize(level, pos, state)
                        ? TickPriority::High : TickPriority::Normal;
                    ScheduleTick(level, pos, state.Block(), 2, priority);
                }
                return;
            }
            if (DiodeIsLocked(level, pos, state)) return;
            const bool on = PoweredOf(state);
            const bool shouldTurnOn = DiodeShouldTurnOn(level, pos, state);
            if (on != shouldTurnOn && !HasScheduledTick(level, pos, state.Block())) {
                TickPriority priority = TickPriority::High;
                if (DiodeShouldPrioritize(level, pos, state)) priority = TickPriority::ExtremelyHigh;
                else if (on)                                   priority = TickPriority::VeryHigh;
                ScheduleTick(level, pos, state.Block(), DiodeGetDelay(state), priority);
            }
        }

        void DiodeTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            if (state.Is(BlockID::Comparator)) {
                ComparatorRefreshOutputState(level, pos, state);
                return;
            }
            if (DiodeIsLocked(level, pos, state)) return;
            const bool on = PoweredOf(state);
            const bool shouldTurnOn = DiodeShouldTurnOn(level, pos, state);
            if (on && !shouldTurnOn) {
                SetBlockClients(level, pos, WithPowered(state, false));
            } else if (!on) {
                SetBlockClients(level, pos, WithPowered(state, true));
                if (!shouldTurnOn) {
                    ScheduleTick(level, pos, state.Block(), DiodeGetDelay(state), TickPriority::VeryHigh);
                }
            }
        }

        void DiodeNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                  BlockID, bool) {
            if (!StateAt(level, pos).Is(state.Block())) return;
            if (DiodeCanSurviveOn(level, Below(pos))) {
                if (RedstonePlus::Enabled()) { Defer(level, pos); return; }
                DiodeCheckTickOnNeighbor(level, pos, state);
            } else {
                DropBlockLoot(level, pos, state);
                RemoveBlock(level, pos, false);
                for (Direction d : kAllDirections) level.UpdateNeighborsAt(Relative(pos, d), state.Block());
            }
        }

        void DiodeOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockState, bool) {
            DiodeUpdateNeighborsInFront(level, pos, state);
        }

        void DiodeAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool movedByPiston) {
            if (!movedByPiston) DiodeUpdateNeighborsInFront(level, pos, state);
        }

        // RepeaterBlock.updateShape / ComparatorBlock.updateShape.
        bool DiodeUpdateShape(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                              Direction toNeighbour, BlockID, BlockState& outState, ScheduledTickAccess*) {
            if (toNeighbour == Direction::Down && !DiodeCanSurviveOn(level, Below(pos))) {
                outState = BlockState{};
                return true;
            }
            if (state.Is(BlockID::Repeater) &&
                AxisOf(toNeighbour) != AxisOf(HorizontalFacingOf(state))) {
                const BlockState next = WithBool(state, PropertyId::LOCKED, RepeaterIsLocked(level, pos, state));
                if (next == state) return false;
                outState = next;
                return true;
            }
            return false;
        }

        UseResult RepeaterUse(ILevelWrite* level, const glm::ivec3& pos, IUsePlayer* player, const BlockHitResult&) {
            if (!level || !player) return UseResult::Pass;
            const BlockState state = StateAt(*level, pos);
            if (!state.Is(BlockID::Repeater)) return UseResult::Pass;
            // state.cycle(DELAY): 1→2→3→4→1.
            const int next = (state.GetIndex(PropertyId::DELAY) + 1) % 4;
            SetBlockAndUpdate(*level, pos, state.SetIndex(PropertyId::DELAY, next));
            return UseResult::Success;
        }

        UseResult ComparatorUse(ILevelWrite* level, const glm::ivec3& pos, IUsePlayer* player, const BlockHitResult&) {
            if (!level || !player) return UseResult::Pass;
            BlockState state = StateAt(*level, pos);
            if (!state.Is(BlockID::Comparator)) return UseResult::Pass;
            state = state.SetIndex(PropertyId::MODE_COMPARATOR, ComparatorIsSubtract(state) ? 0 : 1);
            const float pitch = ComparatorIsSubtract(state) ? 0.55f : 0.5f;
            // MC ComparatorBlock.useWithoutItem:117 — the player hears their
            // own click through prediction, everyone else from the server.
            level->PlaySound(player, pos, SoundEvents::COMPARATOR_CLICK, SoundSource::Blocks, 0.3f, pitch);
            SetBlockClients(*level, pos, state);
            if (!level->IsClientSide() && StateAt(*level, pos).Is(BlockID::Comparator)) {
                ComparatorRefreshOutputState(*level, pos, state);
            }
            return UseResult::Success;
        }

        // ── ObserverBlock ────────────────────────────────────────────────

        void ObserverUpdateNeighborsInFront(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            const Direction direction = FacingOf(state);
            const glm::ivec3 oppositePos = Relative(pos, Opposite(direction));
            level.NeighborChanged(oppositePos, BlockID::Observer);
            level.UpdateNeighborsAtExceptFromFacing(oppositePos, BlockID::Observer, direction);
        }

        void ObserverTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            if (PoweredOf(state)) {
                SetBlockClients(level, pos, WithPowered(state, false));
            } else {
                SetBlockClients(level, pos, WithPowered(state, true));
                ScheduleTick(level, pos, BlockID::Observer, 2);
            }
            ObserverUpdateNeighborsInFront(level, pos, state);
        }

        bool ObserverUpdateShape(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                                 Direction toNeighbour, BlockID, BlockState&, ScheduledTickAccess* ticks) {
            if (FacingOf(state) == toNeighbour && !PoweredOf(state)) {
                // startSignal: server only, and only if nothing is pending.
                (void)level;
                if (ticks && !ticks->HasScheduledTick(pos, BlockID::Observer)) {
                    ticks->ScheduleTick(pos, BlockID::Observer, 2);
                }
            }
            return false;
        }

        int ObserverGetSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction direction) {
            return FacingOf(state) == direction ? (PoweredOf(state) ? 15 : 0) : 0;
        }

        void ObserverOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                             BlockState oldState, bool) {
            if (state.Block() == oldState.Block()) return;
            if (!level.IsClientSide() && PoweredOf(state) && !HasScheduledTick(level, pos, BlockID::Observer)) {
                const BlockState newState = WithPowered(state, false);
                // flag 18 = UPDATE_CLIENTS | UPDATE_KNOWN_SHAPE.
                level.SetBlock(pos.x, pos.y, pos.z, newState,
                               World::UpdateFlags::UpdateClients | World::UpdateFlags::KnownShape);
                ObserverUpdateNeighborsInFront(level, pos, newState);
            }
        }

        void ObserverAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool) {
            if (PoweredOf(state) && HasScheduledTick(level, pos, BlockID::Observer)) {
                ObserverUpdateNeighborsInFront(level, pos, WithPowered(state, false));
            }
        }

        // ── RedstoneLampBlock ────────────────────────────────────────────

        void LampCheck(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            const bool isLit = LitOf(state);
            if (isLit != HasNeighborSignal(level, pos)) {
                if (isLit) ScheduleTick(level, pos, BlockID::RedstoneLamp, 4);
                else       SetBlockClients(level, pos, WithLit(state, true));
            }
        }

        void LampNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockID, bool) {
            if (level.IsClientSide()) return;
            if (RedstonePlus::Enabled()) { Defer(level, pos); return; }
            const bool isLit = LitOf(state);
            if (isLit != HasNeighborSignal(level, pos)) {
                if (isLit) ScheduleTick(level, pos, BlockID::RedstoneLamp, 4);
                else       SetBlockClients(level, pos, WithLit(state, true));
            }
        }

        void LampTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            if (LitOf(state) && !HasNeighborSignal(level, pos)) {
                SetBlockClients(level, pos, WithLit(state, false));
            }
        }

        // ── DisplayBlock (redstone_plus) ─────────────────────────────────
        //
        // An addressable colour pixel with its own memory, so a display is a
        // grid of these with one dust line per row and one per column instead
        // of a latch and taps per pixel. Three colour bits (red, green, blue)
        // and two operations per block, one for its north&west line pair and
        // one for south&east (op_nw / op_se):
        //   set_X    the bit turns on while both lines are powered
        //   clear_X  the bit turns off while both are powered
        //   show_X   the bit follows the pair (a live, unstored colour)
        //   check_X  the pixel's top block emits strong power upward while
        //            both are powered AND the bit is on (a read-back)
        // A CONNECTED GROUP of display blocks (face neighbours) is ONE pixel:
        // the bits are shared, written to every block of the group, so a
        // pillar shows its colour along its whole height and a chain of
        // display blocks can carry a pixel away from its lines to a wall. An
        // operation block reads its row lines on its own north and south
        // faces and its column lines on the west and east faces of the block
        // TWO above it (when that is a display block), so rows and columns
        // cross at different heights (dust lines at one height cannot cross);
        // otherwise it reads its own faces. Dust powered under any block of
        // the group clears every bit. The check output: every block of the
        // group with no display block above it powers what sits on it. A face
        // reads any adjacent dust that carries power, whatever its shape, and
        // any other neighbour that signals toward it — a dust line running
        // PAST a face drives it, which vanilla dust (it powers only what it
        // points at) never does; that is what makes line-per-row addressing
        // impossible with lamps and possible here. Evaluated like the lamp:
        // deferred to the end of the cascade under the rule, immediately
        // otherwise. The stored bits are read from the group's lowest block
        // (lowest y, then z, then x) before the operations apply.
        //
        //   latch    a frame-buffer pixel: while the pair's column face (the clock)
        //            is powered, the group copies EVERY colour bit from the display
        //            group touching the op block's row face (north for op_nw, south
        //            for op_se). A latch block never joins the group across that
        //            data face, so the buffer is its own pixel, sampled only when
        //            clocked — a display that shows complete frames whatever order
        //            the logic behind it updates in.

        // op_nw / op_se value order (GeneratedBlockStates): none, then set /
        // clear / show / check, each as r, g, b.
        constexpr int kDisplayOps = 14;                     // ... then latch
        constexpr int kDisplayLatch = 13;

        // The data face a latch op block reads its source through (and does
        // not join a group across): op_nw -> north, op_se -> south.
        bool DisplayLatchFace(BlockState s, Direction d) {
            if (d == Direction::North) return s.GetIndex(PropertyId::OP_NW) == kDisplayLatch;
            if (d == Direction::South) return s.GetIndex(PropertyId::OP_SE) == kDisplayLatch;
            return false;
        }

        int DisplayBitsOf(BlockState s) {
            return (BoolOf(s, PropertyId::RED) ? 1 : 0) | (BoolOf(s, PropertyId::GREEN) ? 2 : 0) |
                   (BoolOf(s, PropertyId::BLUE) ? 4 : 0);
        }
        BlockState DisplayWithBits(BlockState s, int bits) {
            s = WithBool(s, PropertyId::RED, (bits & 1) != 0);
            s = WithBool(s, PropertyId::GREEN, (bits & 2) != 0);
            return WithBool(s, PropertyId::BLUE, (bits & 4) != 0);
        }
        bool DisplayFaceIn(const IBlockAccess& level, const glm::ivec3& pos, Direction d) {
            const glm::ivec3 np = Relative(pos, d);
            const BlockState ns = StateAt(level, np);
            if (ns.Is(BlockID::RedstoneWire)) return PowerOf(ns) > 0;
            return HasSignal(level, np, d);
        }
        // The group containing `at`, lowest block first (y, then z, then x).
        // Capped: a group is a pixel and its chain, a few hundred blocks; a
        // runaway flood (someone building a floor of them) stops here.
        constexpr size_t kDisplayGroupCap = 4096;
        std::vector<glm::ivec3> DisplayGroup(const IBlockAccess& level, const glm::ivec3& at) {
            std::vector<glm::ivec3> group;
            std::unordered_set<uint64_t> seen;
            group.push_back(at); seen.insert(PosKey(at));
            for (size_t i = 0; i < group.size() && group.size() < kDisplayGroupCap; ++i) {
                const BlockState ps = StateAt(level, group[i]);
                for (Direction d : kAllDirections) {
                    const glm::ivec3 n = Relative(group[i], d);
                    const BlockState ns = StateAt(level, n);
                    if (!ns.Is(BlockID::DisplayBlock)) continue;
                    // a latch's data face is a group boundary, seen from either side
                    if (DisplayLatchFace(ps, d) || DisplayLatchFace(ns, Opposite(d))) continue;
                    if (seen.insert(PosKey(n)).second) group.push_back(n);
                }
            }
            std::sort(group.begin(), group.end(), [](const glm::ivec3& a, const glm::ivec3& b) {
                return std::tie(a.y, a.z, a.x) < std::tie(b.y, b.z, b.x);
            });
            return group;
        }

        void DisplayEvaluate(ILevelWrite& level, const glm::ivec3& at) {
            if (level.IsClientSide()) return;
            if (!StateAt(level, at).Is(BlockID::DisplayBlock)) return;
            const std::vector<glm::ivec3> run = DisplayGroup(level, at);
            int bits = DisplayBitsOf(StateAt(level, run[0]));
            bool out = false;
            for (const glm::ivec3& p : run) {
                if (DisplayFaceIn(level, p, Direction::Down)) { bits = 0; break; }
            }
            for (size_t i = 0; i < run.size(); ++i) {
                const BlockState s = StateAt(level, run[i]);
                const glm::ivec3 twoUp = run[i] + glm::ivec3(0, 2, 0);
                const glm::ivec3 colAt = StateAt(level, twoUp).Is(BlockID::DisplayBlock) ? twoUp : run[i];
                const int ops[2] = { s.GetIndex(PropertyId::OP_NW), s.GetIndex(PropertyId::OP_SE) };
                const bool hit[2] = {
                    DisplayFaceIn(level, run[i], Direction::North) && DisplayFaceIn(level, colAt, Direction::West),
                    DisplayFaceIn(level, run[i], Direction::South) && DisplayFaceIn(level, colAt, Direction::East),
                };
                for (int k = 0; k < 2; ++k) {
                    const int op = ops[k];
                    if (op <= 0 || op >= kDisplayOps) continue;
                    if (op == kDisplayLatch) {
                        const bool clock = DisplayFaceIn(level, colAt, k == 0 ? Direction::West : Direction::East);
                        if (!clock) continue;
                        const BlockState src = StateAt(level, Relative(run[i], k == 0 ? Direction::North : Direction::South));
                        bits = src.Is(BlockID::DisplayBlock) ? DisplayBitsOf(src) : 0;
                        continue;
                    }
                    const int mask = 1 << ((op - 1) % 3);
                    switch ((op - 1) / 3) {
                        case 0: if (hit[k]) bits |= mask; break;                    // set
                        case 1: if (hit[k]) bits &= ~mask; break;                   // clear
                        case 2: if (hit[k]) bits |= mask; else bits &= ~mask; break; // show
                        default: if (hit[k] && (bits & mask)) out = true; break;   // check
                    }
                }
            }
            for (size_t i = 0; i < run.size(); ++i) {
                const BlockState s = StateAt(level, run[i]);
                const bool top = !StateAt(level, Relative(run[i], Direction::Up)).Is(BlockID::DisplayBlock);
                const BlockState ns = WithBool(DisplayWithBits(s, bits), PropertyId::POWERED, out && top);
                if (ns != s) SetBlockAndUpdate(level, run[i], ns);
            }
        }

        void DisplayNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState, BlockID, bool) {
            if (level.IsClientSide()) return;
            if (RedstonePlus::Enabled()) { Defer(level, pos); return; }
            DisplayEvaluate(level, pos);
        }

        void DisplayOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState, BlockState, bool) {
            if (!level.IsClientSide()) DisplayEvaluate(level, pos);
        }

        // A group's top blocks power what sits on them: the dust above asks
        // with Down (the direction from the asker to this block).
        int DisplayGetSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction direction) {
            return (PoweredOf(state) && direction == Direction::Down) ? 15 : 0;
        }
        int DisplayAnalogOutput(ILevelWrite&, const glm::ivec3&, BlockState state, Direction) {
            return DisplayBitsOf(state);
        }

        // Empty hand: cycle op_nw; sneaking cycles op_se.
        UseResult DisplayUse(ILevelWrite* level, const glm::ivec3& pos, IUsePlayer* player, const BlockHitResult&) {
            if (!level || !player) return UseResult::Pass;
            const BlockState state = StateAt(*level, pos);
            if (!state.Is(BlockID::DisplayBlock)) return UseResult::Pass;
            if (!level->IsClientSide()) {
                const PropertyId prop = player->IsSneaking() ? PropertyId::OP_SE : PropertyId::OP_NW;
                SetBlockAndUpdate(*level, pos, state.SetIndex(prop, (state.GetIndex(prop) + 1) % kDisplayOps));
                DisplayEvaluate(*level, pos);
            }
            return UseResult::Success;
        }

        // ── PoweredBlock (block of redstone) ─────────────────────────────

        int RedstoneBlockGetSignal(const IBlockAccess&, const glm::ivec3&, BlockState, Direction) {
            return 15;
        }

        // ── LeverBlock / ButtonBlock (FaceAttachedHorizontalDirectionalBlock)

        void FaceAttachedUpdateNeighbours(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            const Direction front = Opposite(ConnectedDirectionOf(state));
            level.UpdateNeighborsAt(pos, state.Block());
            level.UpdateNeighborsAt(Relative(pos, front), state.Block());
        }

        int FaceAttachedGetSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction) {
            return PoweredOf(state) ? 15 : 0;
        }
        int FaceAttachedGetDirectSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction direction) {
            return PoweredOf(state) && ConnectedDirectionOf(state) == direction ? 15 : 0;
        }

        void FaceAttachedAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool movedByPiston) {
            if (!movedByPiston && PoweredOf(state)) FaceAttachedUpdateNeighbours(level, pos, state);
        }

        // LeverBlock.pull.
        void LeverPull(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            state = WithPowered(state, !PoweredOf(state));
            SetBlockAndUpdate(level, pos, state);
            FaceAttachedUpdateNeighbours(level, pos, state);
            // MC LeverBlock.playSound: `pull(state, level, pos, null)` from
            // useWithoutItem, so every client hears it from the server.
            level.PlaySound(nullptr, pos, SoundEvents::LEVER_CLICK, SoundSource::Blocks, 0.3f,
                            PoweredOf(state) ? 0.6f : 0.5f);
        }

        UseResult LeverUse(ILevelWrite* level, const glm::ivec3& pos, IUsePlayer* player, const BlockHitResult&) {
            if (!level || !player) return UseResult::Pass;
            const BlockState state = StateAt(*level, pos);
            if (!state.Is(BlockID::Lever)) return UseResult::Pass;
            // MC: the client only spawns the particle; the server pulls.
            if (!level->IsClientSide()) LeverPull(*level, pos, state);
            return UseResult::Success;
        }

        // MC ButtonBlock.playSound: `level.playSound(pressed ? player : null,
        // pos, getSound(pressed), BLOCKS)` — the BlockSetType's click.
        void ButtonPlaySound(ILevelWrite& level, IUsePlayer* player, BlockID id, const glm::ivec3& pos,
                             bool pressed) {
            const BlockSetType* set = BlockSetTypeOf(id);
            if (!set) return;
            level.PlaySound(pressed ? SoundExcept(player) : SoundExcept(nullptr), pos,
                            pressed ? set->buttonClickOn : set->buttonClickOff, SoundSource::Blocks);
        }

        // ButtonBlock.press.
        void ButtonPress(ILevelWrite& level, const glm::ivec3& pos, BlockState state, IUsePlayer* player) {
            SetBlockAndUpdate(level, pos, WithPowered(state, true));
            FaceAttachedUpdateNeighbours(level, pos, state);
            ScheduleTick(level, pos, state.Block(), ButtonTicksToStayPressed(state.Block()));
            ButtonPlaySound(level, player, state.Block(), pos, true);
        }

        UseResult ButtonUse(ILevelWrite* level, const glm::ivec3& pos, IUsePlayer* player, const BlockHitResult&) {
            if (!level || !player) return UseResult::Pass;
            const BlockState state = StateAt(*level, pos);
            if (!IsButtonBlock(state.Block())) return UseResult::Pass;
            if (PoweredOf(state)) return UseResult::Consume;
            ButtonPress(*level, pos, state, player);
            return UseResult::Success;
        }

        // ButtonBlock.checkPressed: an arrow resting in the button keeps it
        // down; the scheduled tick releases it otherwise.
        void ButtonCheckPressed(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            bool shouldBePressed = false;
            if (IsWoodenButton(state.Block())) {
                const auto shapes = BlockRegistry::GetBlockShapeSet(state);
                glm::vec3 lo(1.0f), hi(0.0f);
                for (const auto& s : shapes) { lo = glm::min(lo, s.min); hi = glm::max(hi, s.max); }
                if (shapes.count == 0) { lo = glm::vec3(0.0f); hi = glm::vec3(1.0f); }
                const AABBd box = AABBd::FromMinMax(glm::dvec3(pos) + glm::dvec3(lo),
                                                    glm::dvec3(pos) + glm::dvec3(hi));
                shouldBePressed = CountEntitiesInBox(level, box).arrows > 0;
            }
            const bool wasPressed = PoweredOf(state);
            if (shouldBePressed != wasPressed) {
                SetBlockAndUpdate(level, pos, WithPowered(state, shouldBePressed));
                FaceAttachedUpdateNeighbours(level, pos, state);
                ButtonPlaySound(level, nullptr, state.Block(), pos, shouldBePressed);
            }
            if (shouldBePressed) {
                ScheduleTick(level, pos, state.Block(), ButtonTicksToStayPressed(state.Block()));
            }
        }

        void ButtonTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            if (PoweredOf(state)) ButtonCheckPressed(level, pos, state);
        }

        void ButtonEntityInside(ILevelWrite& level, const glm::ivec3& pos, BlockState state, Entity& entity) {
            if (level.IsClientSide() || !IsWoodenButton(state.Block()) || PoweredOf(state)) return;
            const EntityTypeId t = entity.GetType();
            if (t != EntityTypeId::Arrow && t != EntityTypeId::Trident) return;
            ButtonCheckPressed(level, pos, state);
        }

        // ── Pressure plates (BasePressurePlateBlock and the two subclasses)

        int PlateSignalForState(BlockState state) {
            return IsWeightedPressurePlate(state.Block()) ? PowerOf(state) : (PoweredOf(state) ? 15 : 0);
        }
        BlockState PlateSetSignalForState(BlockState state, int signal) {
            return IsWeightedPressurePlate(state.Block()) ? WithPower(state, signal)
                                                           : WithPowered(state, signal > 0);
        }
        int PlatePressedTime(BlockID id) { return IsWeightedPressurePlate(id) ? 10 : 20; }

        // TOUCH_AABB = column(14, 0, 4).
        AABBd PlateTouchBox(const glm::ivec3& pos) {
            return CellBox(pos, 1.0 / 16, 0.0, 1.0 / 16, 15.0 / 16, 4.0 / 16, 15.0 / 16);
        }

        int PlateGetSignalStrength(ILevelWrite& level, const glm::ivec3& pos, BlockID id) {
            const EntityCount count = CountEntitiesInBox(level, PlateTouchBox(pos));
            if (IsWeightedPressurePlate(id)) {
                const int maxWeight = WeightedPlateMaxWeight(id);
                const int n = std::min(count.all(), maxWeight);
                if (n <= 0) return 0;
                const float percent = static_cast<float>(std::min(maxWeight, n)) / static_cast<float>(maxWeight);
                return static_cast<int>(std::ceil(percent * 15.0f));
            }
            const int n = PressurePlateMobsOnly(id) ? count.living : count.all();
            return n > 0 ? 15 : 0;
        }

        // MC BasePressurePlateBlock.checkPressed:86/89 — the BlockSetType's
        // click, for everyone (except = null).
        void PlatePlaySound(ILevelWrite& level, BlockID id, const glm::ivec3& pos, bool on) {
            const BlockSetType* set = BlockSetTypeOf(id);
            if (!set) return;
            level.PlaySound(nullptr, pos, on ? set->pressurePlateClickOn : set->pressurePlateClickOff,
                            SoundSource::Blocks);
        }

        void PlateUpdateNeighbours(ILevelWrite& level, const glm::ivec3& pos, BlockID id) {
            level.UpdateNeighborsAt(pos, id);
            level.UpdateNeighborsAt(Below(pos), id);
        }

        void PlateCheckPressed(ILevelWrite& level, const glm::ivec3& pos, BlockState state, int oldSignal) {
            const int signal = PlateGetSignalStrength(level, pos, state.Block());
            const bool wasPressed = oldSignal > 0;
            const bool isPressed  = signal > 0;
            if (oldSignal != signal) {
                SetBlockClients(level, pos, PlateSetSignalForState(state, signal));
                PlateUpdateNeighbours(level, pos, state.Block());
            }
            if (!isPressed && wasPressed)      PlatePlaySound(level, state.Block(), pos, false);
            else if (isPressed && !wasPressed) PlatePlaySound(level, state.Block(), pos, true);
            if (isPressed) ScheduleTick(level, pos, state.Block(), PlatePressedTime(state.Block()));
        }

        void PlateTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            const int signal = PlateSignalForState(state);
            if (signal > 0) PlateCheckPressed(level, pos, state, signal);
        }

        void PlateAnyInside(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            if (level.IsClientSide()) return;
            const int signal = PlateSignalForState(state);
            if (signal == 0) PlateCheckPressed(level, pos, state, signal);
        }
        void PlateEntityInside(ILevelWrite& level, const glm::ivec3& pos, BlockState state, Entity&) {
            PlateAnyInside(level, pos, state);
        }

        void PlateAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool movedByPiston) {
            if (!movedByPiston && PlateSignalForState(state) > 0) PlateUpdateNeighbours(level, pos, state.Block());
        }

        int PlateGetSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction) {
            return PlateSignalForState(state);
        }
        int PlateGetDirectSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction direction) {
            return direction == Direction::Up ? PlateSignalForState(state) : 0;
        }

        bool PlateCanSurvive(const IBlockAccess& level, const glm::ivec3& pos) {
            const glm::ivec3 below = Below(pos);
            return CanSupportRigidBlock(level, below) || CanSupportCenter(level, below);
        }

        bool PlateUpdateShape(const IBlockAccess& level, const glm::ivec3& pos, BlockState,
                              Direction toNeighbour, BlockID, BlockState& outState, ScheduledTickAccess*) {
            if (toNeighbour == Direction::Down && !PlateCanSurvive(level, pos)) {
                outState = BlockState{};
                return true;
            }
            return false;
        }

        // ── NoteBlock ────────────────────────────────────────────────────

        // NoteBlockInstrument.worksAboveNoteBlock: the mob heads and the
        // custom head play from ABOVE the block; every base-block instrument
        // is read from below.
        bool InstrumentWorksAbove(std::string_view name) {
            return name == "zombie" || name == "skeleton" || name == "creeper" ||
                   name == "dragon" || name == "wither_skeleton" || name == "piglin" ||
                   name == "custom_head";
        }
        bool InstrumentIsTunable(std::string_view name) { return !InstrumentWorksAbove(name); }

        BlockState WithInstrument(BlockState state, std::string_view name) {
            const BlockState next = state.SetName(PropertyId::INSTRUMENT, name);
            // An instrument this build's state table lacks (26.3's trumpets)
            // falls back to the harp rather than leaving the state untouched.
            return next == state && state.GetName(PropertyId::INSTRUMENT) != name
                ? state.SetName(PropertyId::INSTRUMENT, "harp") : next;
        }

        void NoteBlockPlayNote(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            if (InstrumentWorksAbove(state.GetName(PropertyId::INSTRUMENT)) ||
                level.GetBlock(pos.x, pos.y + 1, pos.z) == BlockID::Air) {
                level.BlockEvent(pos, BlockID::NoteBlock, 0, 0);
            }
        }

        bool NoteBlockUpdateShape(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                                  Direction toNeighbour, BlockID, BlockState& outState, ScheduledTickAccess*) {
            if (AxisOf(toNeighbour) != Axis::Y) return false;
            const BlockState next = NoteBlockWithInstrument(level, pos, state);
            if (next == state) return false;
            outState = next;
            return true;
        }

        void NoteBlockNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockID, bool) {
            const bool signal = HasNeighborSignal(level, pos);
            if (signal != PoweredOf(state)) {
                if (signal) NoteBlockPlayNote(level, pos, state);
                SetBlockAndUpdate(level, pos, WithPowered(state, signal));
            }
        }

        UseResult NoteBlockUse(ILevelWrite* level, const glm::ivec3& pos, IUsePlayer* player, const BlockHitResult&) {
            if (!level || !player) return UseResult::Pass;
            BlockState state = StateAt(*level, pos);
            if (!state.Is(BlockID::NoteBlock)) return UseResult::Pass;
            if (!level->IsClientSide()) {
                state = state.SetIndex(PropertyId::NOTE, (state.GetIndex(PropertyId::NOTE) + 1) % 25);
                SetBlockAndUpdate(*level, pos, state);
                NoteBlockPlayNote(*level, pos, state);
            }
            return UseResult::Success;
        }

        void NoteBlockAttack(ILevelWrite& level, const glm::ivec3& pos) {
            if (level.IsClientSide()) return;
            const BlockState state = StateAt(level, pos);
            if (state.Is(BlockID::NoteBlock)) NoteBlockPlayNote(level, pos, state);
        }

        float PitchFromNote(int note) {
            return static_cast<float>(std::pow(2.0, static_cast<double>(note - 12) / 12.0));
        }

        // MC NoteBlockInstrument.getSoundEvent: the base-block instruments are
        // block.note_block.<name>; the mob heads imitate (their ids differ
        // from the serialised names).
        std::string NoteBlockSoundEvent(std::string_view instrument) {
            if (instrument == "zombie")          return SoundEvents::NOTE_BLOCK_IMITATE_ZOMBIE;
            if (instrument == "skeleton")        return SoundEvents::NOTE_BLOCK_IMITATE_SKELETON;
            if (instrument == "creeper")         return SoundEvents::NOTE_BLOCK_IMITATE_CREEPER;
            if (instrument == "dragon")          return SoundEvents::NOTE_BLOCK_IMITATE_ENDER_DRAGON;
            if (instrument == "wither_skeleton") return SoundEvents::NOTE_BLOCK_IMITATE_WITHER_SKELETON;
            if (instrument == "piglin")          return SoundEvents::NOTE_BLOCK_IMITATE_PIGLIN;
            return "block.note_block." + std::string(instrument);
        }

        bool NoteBlockTriggerEvent(ILevelWrite& level, const glm::ivec3& pos, BlockState state, int, int) {
            const std::string_view instrument = state.GetName(PropertyId::INSTRUMENT);
            const float pitch = InstrumentIsTunable(instrument)
                ? PitchFromNote(state.GetIndex(PropertyId::NOTE)) : 1.0f;
            // MC: the custom head's sound is the skull's note_block_sound
            // component (SkullBlockEntity.getNoteBlockSound); skulls carry no
            // such component here, which MC answers the same way — silence.
            if (instrument == "custom_head") return false;
            // MC NoteBlock.triggerEvent:137 — playSeededSound(null, centre,
            // instrument, RECORDS, 3.0F, pitch, seed): the Jukebox/Note Blocks
            // slider, and a 48-block reach at volume 3.
            level.PlaySound(nullptr, pos, NoteBlockSoundEvent(instrument), SoundSource::Records, 3.0f, pitch);
            return true;
        }

        // ── TargetBlock ──────────────────────────────────────────────────

        double Frac(double v) { return v - std::floor(v); }

        int TargetGetRedstoneStrength(Direction hitDirection, const glm::dvec3& hitLocation) {
            const double distX = std::abs(Frac(hitLocation.x) - 0.5);
            const double distY = std::abs(Frac(hitLocation.y) - 0.5);
            const double distZ = std::abs(Frac(hitLocation.z) - 0.5);
            const Axis axis = AxisOf(hitDirection);
            double distance;
            if (axis == Axis::Y)      distance = std::max(distX, distZ);
            else if (axis == Axis::Z) distance = std::max(distX, distY);
            else                      distance = std::max(distY, distZ);
            return std::max(1, static_cast<int>(std::ceil(15.0 * std::clamp((0.5 - distance) / 0.5, 0.0, 1.0))));
        }

        void TargetOnProjectileHit(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                   const glm::dvec3& hitPos, Direction face, Entity& projectile) {
            const int redstoneStrength = TargetGetRedstoneStrength(face, hitPos);
            const EntityTypeId t = projectile.GetType();
            const int duration = (t == EntityTypeId::Arrow || t == EntityTypeId::Trident) ? 20 : 8;
            if (!HasScheduledTick(level, pos, BlockID::Target)) {
                SetBlockAndUpdate(level, pos, WithPower(state, redstoneStrength));
                ScheduleTick(level, pos, BlockID::Target, duration);
            }
        }

        void TargetTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            if (PowerOf(state) != 0) SetBlockAndUpdate(level, pos, WithPower(state, 0));
        }

        int TargetGetSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction) {
            return PowerOf(state);
        }

        void TargetOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockState oldState, bool) {
            if (level.IsClientSide() || state.Block() == oldState.Block()) return;
            if (PowerOf(state) > 0 && !HasScheduledTick(level, pos, BlockID::Target)) {
                level.SetBlock(pos.x, pos.y, pos.z, WithPower(state, 0),
                               World::UpdateFlags::UpdateClients | World::UpdateFlags::KnownShape);
            }
        }

        // ── DaylightDetectorBlock ────────────────────────────────────────

        UseResult DaylightDetectorUse(ILevelWrite* level, const glm::ivec3& pos, IUsePlayer* player, const BlockHitResult&) {
            if (!level || !player) return UseResult::Pass;
            const BlockState state = StateAt(*level, pos);
            if (!state.Is(BlockID::DaylightDetector)) return UseResult::Pass;
            if (!level->IsClientSide()) {
                const BlockState newState = WithBool(state, PropertyId::INVERTED, !BoolOf(state, PropertyId::INVERTED));
                SetBlockClients(*level, pos, newState);
                DaylightDetectorUpdateSignalStrength(*level, pos, newState);
            }
            return UseResult::Success;
        }

        int DaylightDetectorGetSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction) {
            return PowerOf(state);
        }

        // ── RedStoneOreBlock ─────────────────────────────────────────────

        void RedstoneOreInteract(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            if (!LitOf(state)) SetBlockAndUpdate(level, pos, WithLit(state, true));
        }
        void RedstoneOreAttack(ILevelWrite& level, const glm::ivec3& pos) {
            RedstoneOreInteract(level, pos, StateAt(level, pos));
        }
        UseResult RedstoneOreUseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                                       IUsePlayer*, uint32_t, const BlockHitResult&) {
            if (!level) return UseResult::Pass;
            if (!level->IsClientSide()) RedstoneOreInteract(*level, pos, StateAt(*level, pos));
            // MC: PASS when the held item is a placeable block (so the
            // placement still happens), SUCCESS otherwise.
            return stack.itemId < PURE_ITEM_BASE && !stack.IsEmpty() ? UseResult::Pass : UseResult::Success;
        }
        bool RedstoneOreIsRandomlyTicking(BlockState state) { return LitOf(state); }
        void RedstoneOreRandomTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            if (LitOf(state)) SetBlockAndUpdate(level, pos, WithLit(state, false));
        }

        // ── CopperBulbBlock ──────────────────────────────────────────────

        void CopperBulbCheckAndFlip(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            const bool signal = HasNeighborSignal(level, pos);
            if (signal == PoweredOf(state)) return;
            BlockState newState = state;
            if (!PoweredOf(state)) {
                newState = WithLit(newState, !LitOf(newState));
                // MC CopperBulbBlock.checkAndFlip:47.
                level.PlaySound(nullptr, pos, LitOf(newState) ? SoundEvents::COPPER_BULB_TURN_ON
                                                              : SoundEvents::COPPER_BULB_TURN_OFF,
                                SoundSource::Blocks);
            }
            SetBlockAndUpdate(level, pos, WithPowered(newState, signal));
        }
        void CopperBulbOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockState oldState, bool) {
            if (oldState.Block() != state.Block() && !level.IsClientSide()) CopperBulbCheckAndFlip(level, pos, state);
        }
        void CopperBulbNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockID, bool) {
            if (!level.IsClientSide()) CopperBulbCheckAndFlip(level, pos, state);
        }
        int CopperBulbAnalogOutput(ILevelWrite& level, const glm::ivec3& pos, BlockState, Direction) {
            return LitOf(StateAt(level, pos)) ? 15 : 0;
        }

        // ── TntBlock.neighborChanged ─────────────────────────────────────

        void TntNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState, BlockID, bool) {
            if (HasNeighborSignal(level, pos) && TntPrime(level, pos, nullptr)) {
                RemoveBlock(level, pos, false);
            }
        }

        // ── DoorBlock / TrapDoorBlock / FenceGateBlock.neighborChanged ───

        // The door / trapdoor sounds of the block's BlockSetType (MC
        // DoorBlock.playSound / TrapDoorBlock.playSound read this.type).
        const char* DoorSound(BlockID id, bool open) {
            const BlockSetType* set = BlockSetTypeOf(id);
            return set ? (open ? set->doorOpen : set->doorClose) : "";
        }
        const char* TrapdoorSound(BlockID id, bool open) {
            const BlockSetType* set = BlockSetTypeOf(id);
            return set ? (open ? set->trapdoorOpen : set->trapdoorClose) : "";
        }

        void DoorNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockID sourceBlock, bool) {
            const bool lower = state.GetName(PropertyId::DOUBLE_BLOCK_HALF) == "lower";
            const bool signal = HasNeighborSignal(level, pos) ||
                                HasNeighborSignal(level, Relative(pos, lower ? Direction::Up : Direction::Down));
            // `!this.defaultBlockState().is(block)`: the other half's own
            // update is not a reason to re-evaluate.
            if (sourceBlock == state.Block()) return;
            if (signal != PoweredOf(state)) {
                if (signal != OpenOf(state)) {
                    // MC DoorBlock.neighborChanged:185 → playSound(null, ...).
                    level.PlaySound(nullptr, pos, DoorSound(state.Block(), signal), SoundSource::Blocks,
                                    1.0f, RandomFloat(level) * 0.1f + 0.9f);
                }
                SetBlockClients(level, pos, WithOpen(WithPowered(state, signal), signal));
            }
        }

        void TrapdoorNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockID, bool) {
            if (level.IsClientSide()) return;
            const bool signal = HasNeighborSignal(level, pos);
            if (signal != PoweredOf(state)) {
                if (OpenOf(state) != signal) {
                    state = WithOpen(state, signal);
                    // MC TrapDoorBlock.neighborChanged:108 → playSound(null, ...).
                    level.PlaySound(nullptr, pos, TrapdoorSound(state.Block(), signal), SoundSource::Blocks,
                                    1.0f, RandomFloat(level) * 0.1f + 0.9f);
                }
                SetBlockClients(level, pos, WithPowered(state, signal));
            }
        }

        void FenceGateNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockID, bool) {
            if (level.IsClientSide()) return;
            const bool hasPower = HasNeighborSignal(level, pos);
            if (PoweredOf(state) != hasPower) {
                SetBlockClients(level, pos, WithOpen(WithPowered(state, hasPower), hasPower));
                if (OpenOf(state) != hasPower) {
                    // MC FenceGateBlock.neighborChanged:151 — the WoodType's gate.
                    if (const WoodType* wood = WoodTypeOf(state.Block())) {
                        level.PlaySound(nullptr, pos, hasPower ? wood->fenceGateOpen : wood->fenceGateClose,
                                        SoundSource::Blocks, 1.0f, RandomFloat(level) * 0.1f + 0.9f);
                    }
                }
            }
        }

        // ── TripWireBlock / TripWireHookBlock ────────────────────────────

        constexpr int kWireDistMax = 42;

        bool TripWireShouldConnectTo(BlockState blockState, Direction direction) {
            if (blockState.Is(BlockID::TripwireHook)) {
                return HorizontalFacingOf(blockState) == Opposite(direction);
            }
            return blockState.Is(BlockID::Tripwire);
        }

        PropertyId TripWireSideProp(Direction d) {
            switch (d) {
                case Direction::North: return PropertyId::NORTH;
                case Direction::East:  return PropertyId::EAST;
                case Direction::South: return PropertyId::SOUTH;
                default:               return PropertyId::WEST;
            }
        }

        void HookNotifyNeighbors(ILevelWrite& level, const glm::ivec3& pos, Direction direction) {
            const Direction front = Opposite(direction);
            level.UpdateNeighborsAt(pos, BlockID::TripwireHook);
            level.UpdateNeighborsAt(Relative(pos, front), BlockID::TripwireHook);
        }

        void HookEmitState(ILevelWrite& level, const glm::ivec3& pos, bool attached, bool powered,
                           bool wasAttached, bool wasPowered) {
            // MC TripWireHookBlock.emitState:176, every one except = null.
            constexpr SoundSource kBlocks = SoundSource::Blocks;
            if (powered && !wasPowered)        level.PlaySound(nullptr, pos, SoundEvents::TRIPWIRE_CLICK_ON, kBlocks, 0.4f, 0.6f);
            else if (!powered && wasPowered)   level.PlaySound(nullptr, pos, SoundEvents::TRIPWIRE_CLICK_OFF, kBlocks, 0.4f, 0.5f);
            else if (attached && !wasAttached) level.PlaySound(nullptr, pos, SoundEvents::TRIPWIRE_ATTACH, kBlocks, 0.4f, 0.7f);
            else if (!attached && wasAttached) level.PlaySound(nullptr, pos, SoundEvents::TRIPWIRE_DETACH, kBlocks, 0.4f,
                                                               1.2f / (RandomFloat(level) * 0.2f + 0.9f));
        }

        void HookOnRemoved(ILevelWrite& level, const glm::ivec3& pos, BlockState state);

        // TripWireHookBlock.calculateState, verbatim in shape.
        void HookCalculateState(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                bool isBeingDestroyed, bool canUpdate, int wireSource,
                                const BlockState* wireSourceState) {
            if (!state.HasProperty(PropertyId::HORIZONTAL_FACING)) return;
            const Direction direction = HorizontalFacingOf(state);
            const bool wasAttached = state.HasProperty(PropertyId::ATTACHED) && BoolOf(state, PropertyId::ATTACHED);
            const bool wasPowered  = state.HasProperty(PropertyId::POWERED)  && PoweredOf(state);
            bool attached = !isBeingDestroyed;
            bool powered = false;
            int receiverPos = 0;
            BlockState wireStates[kWireDistMax];
            bool       wirePresent[kWireDistMax] = {};

            for (int i = 1; i < kWireDistMax; ++i) {
                const glm::ivec3 testPos = Relative(pos, direction, i);
                BlockState wireState = StateAt(level, testPos);
                if (wireState.Is(BlockID::TripwireHook)) {
                    if (HorizontalFacingOf(wireState) == Opposite(direction)) receiverPos = i;
                    break;
                }
                if (!wireState.Is(BlockID::Tripwire) && i != wireSource) {
                    wirePresent[i] = false;
                    attached = false;
                } else {
                    if (i == wireSource && wireSourceState) wireState = *wireSourceState;
                    const bool wireArmed   = !BoolOf(wireState, PropertyId::DISARMED);
                    const bool wirePowered = PoweredOf(wireState);
                    powered |= wireArmed && wirePowered;
                    wireStates[i]  = wireState;
                    wirePresent[i] = true;
                    if (i == wireSource) {
                        ScheduleTick(level, pos, BlockID::TripwireHook, 10);
                        attached &= wireArmed;
                    }
                }
            }

            attached &= receiverPos > 1;
            powered  &= attached;
            const BlockState newState = WithPowered(
                WithBool(BlockStates::Default(BlockID::TripwireHook), PropertyId::ATTACHED, attached), powered);

            if (receiverPos > 0) {
                const glm::ivec3 testPos = Relative(pos, direction, receiverPos);
                const Direction opposite = Opposite(direction);
                SetBlockAndUpdate(level, testPos, WithHorizontalFacing(newState, opposite));
                HookNotifyNeighbors(level, testPos, opposite);
                if (!StateAt(level, pos).Is(BlockID::TripwireHook)) {
                    HookOnRemoved(level, pos, newState);
                    return;
                }
                HookEmitState(level, testPos, attached, powered, wasAttached, wasPowered);
            }

            HookEmitState(level, pos, attached, powered, wasAttached, wasPowered);
            if (!isBeingDestroyed) {
                SetBlockAndUpdate(level, pos, WithHorizontalFacing(newState, direction));
                if (canUpdate) HookNotifyNeighbors(level, pos, direction);
            }

            if (wasAttached != attached) {
                for (int i = 1; i < receiverPos; ++i) {
                    if (!wirePresent[i]) continue;
                    const glm::ivec3 testPos = Relative(pos, direction, i);
                    const BlockState testPosState = StateAt(level, testPos);
                    if (testPosState.Is(BlockID::Tripwire) || testPosState.Is(BlockID::TripwireHook)) {
                        SetBlockAndUpdate(level, testPos, WithBool(wireStates[i], PropertyId::ATTACHED, attached));
                    }
                }
            }
        }

        void HookOnRemoved(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            const bool attached = BoolOf(state, PropertyId::ATTACHED);
            const bool powered  = PoweredOf(state);
            if (attached || powered) HookCalculateState(level, pos, state, true, false, -1, nullptr);
            if (powered) HookNotifyNeighbors(level, pos, HorizontalFacingOf(state));
        }

        void HookTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            HookCalculateState(level, pos, state, false, true, -1, nullptr);
        }

        void HookAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool movedByPiston) {
            if (!movedByPiston) HookOnRemoved(level, pos, state);
        }

        int HookGetSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction) {
            return PoweredOf(state) ? 15 : 0;
        }
        int HookGetDirectSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction direction) {
            if (!PoweredOf(state)) return 0;
            return HorizontalFacingOf(state) == direction ? 15 : 0;
        }

        bool HookCanSurvive(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
            const Direction direction = HorizontalFacingOf(state);
            const glm::ivec3 relative = Relative(pos, Opposite(direction));
            return IsHorizontal(direction) && IsFaceSturdyAt(level, relative, direction);
        }

        bool HookUpdateShape(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                             Direction toNeighbour, BlockID, BlockState& outState, ScheduledTickAccess*) {
            if (Opposite(toNeighbour) == HorizontalFacingOf(state) && !HookCanSurvive(level, pos, state)) {
                outState = BlockState{};
                return true;
            }
            return false;
        }

        // TripWireBlock.updateSource: walk south and west to the hooks.
        void TripWireUpdateSource(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            for (Direction direction : {Direction::South, Direction::West}) {
                for (int i = 1; i < kWireDistMax; ++i) {
                    const glm::ivec3 testPos = Relative(pos, direction, i);
                    const BlockState block = StateAt(level, testPos);
                    if (block.Is(BlockID::TripwireHook)) {
                        if (HorizontalFacingOf(block) == Opposite(direction)) {
                            HookCalculateState(level, testPos, block, false, true, i, &state);
                        }
                        break;
                    }
                    if (!block.Is(BlockID::Tripwire)) break;
                }
            }
        }

        bool TripWireUpdateShape(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                                 Direction toNeighbour, BlockID, BlockState& outState, ScheduledTickAccess*) {
            if (!IsHorizontal(toNeighbour)) return false;
            const BlockState neighbour = StateAt(level, Relative(pos, toNeighbour));
            const BlockState next = WithBool(state, TripWireSideProp(toNeighbour),
                                             TripWireShouldConnectTo(neighbour, toNeighbour));
            if (next == state) return false;
            outState = next;
            return true;
        }

        void TripWireOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockState oldState, bool) {
            if (oldState.Block() != state.Block()) TripWireUpdateSource(level, pos, state);
        }

        void TripWireAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool movedByPiston) {
            if (!movedByPiston) TripWireUpdateSource(level, pos, WithPowered(state, true));
        }

        void TripWireCheckPressed(ILevelWrite& level, const glm::ivec3& pos, bool anyEntityKnown) {
            BlockState state = StateAt(level, pos);
            if (!state.Is(BlockID::Tripwire)) return;
            const bool wasPressed = PoweredOf(state);
            bool shouldBePressed = anyEntityKnown;
            if (!shouldBePressed) {
                const auto shapes = BlockRegistry::GetBlockShapeSet(state);
                glm::vec3 lo(1.0f), hi(0.0f);
                for (const auto& s : shapes) { lo = glm::min(lo, s.min); hi = glm::max(hi, s.max); }
                if (shapes.count == 0) { lo = glm::vec3(0.0f); hi = glm::vec3(1.0f); }
                const AABBd box = AABBd::FromMinMax(glm::dvec3(pos) + glm::dvec3(lo),
                                                    glm::dvec3(pos) + glm::dvec3(hi));
                shouldBePressed = CountEntitiesInBox(level, box).all() > 0;
            }
            if (shouldBePressed != wasPressed) {
                state = WithPowered(state, shouldBePressed);
                SetBlockAndUpdate(level, pos, state);
                TripWireUpdateSource(level, pos, state);
            }
            if (shouldBePressed)  ScheduleTick(level, pos, BlockID::Tripwire, 10);
            else if (wasPressed)  ScheduleTick(level, pos, BlockID::Tripwire, 1);
        }

        void TripWireAnyInside(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            if (level.IsClientSide()) return;
            if (!PoweredOf(state) && !HasScheduledTick(level, pos, BlockID::Tripwire)) {
                TripWireCheckPressed(level, pos, true);
            }
        }
        void TripWireEntityInside(ILevelWrite& level, const glm::ivec3& pos, BlockState state, Entity& entity) {
            if (entity.IsSpectator()) return;
            TripWireAnyInside(level, pos, state);
        }

        void TripWireTick(ILevelWrite& level, const glm::ivec3& pos, BlockState, JavaRandom&) {
            if (PoweredOf(StateAt(level, pos))) TripWireCheckPressed(level, pos, false);
        }

    } // namespace

    // ── Public helpers ─────────────────────────────────────────────────────

    BlockState NoteBlockWithInstrument(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
        const std::string_view instrumentAbove = BlockRegistry::Get(level.GetBlock(pos.x, pos.y + 1, pos.z)).instrument;
        if (InstrumentWorksAbove(instrumentAbove)) return WithInstrument(state, instrumentAbove);
        const std::string_view instrumentBelow = BlockRegistry::Get(level.GetBlock(pos.x, pos.y - 1, pos.z)).instrument;
        return WithInstrument(state, InstrumentWorksAbove(instrumentBelow) ? "harp" : instrumentBelow);
    }

    BlockState TripWirePlacementState(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
        for (Direction d : {Direction::North, Direction::East, Direction::South, Direction::West}) {
            state = WithBool(state, TripWireSideProp(d), TripWireShouldConnectTo(StateAt(level, Relative(pos, d)), d));
        }
        return state;
    }

    BlockState RepeaterPlacementState(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
        return WithBool(state, PropertyId::LOCKED, RepeaterIsLocked(level, pos, state));
    }

    void RedstoneComponentPlacedBy(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
        if (state.Is(BlockID::TripwireHook)) {
            HookCalculateState(level, pos, state, false, false, -1, nullptr);
        } else if (state.Is(BlockID::Piston) || state.Is(BlockID::StickyPiston)) {
            PistonPlacedBy(level, pos, state);
        } else if (IsDiodeBlock(state.Block())) {
            if (DiodeShouldTurnOn(level, pos, state)) ScheduleTick(level, pos, state.Block(), 1);
        }
    }

    // OverworldDimension.timeOfDay → sun angle, for the daylight detector.
    namespace {
        float TimeOfDay(int64_t dayTime) {
            const double d = Frac(static_cast<double>(dayTime) / 24000.0 - 0.25);
            const double e = 0.5 - std::cos(d * 3.141592653589793) / 2.0;
            return static_cast<float>((d * 2.0 + e) / 3.0);
        }
    }

    void DaylightDetectorUpdateSignalStrength(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
        auto* world = dynamic_cast<World*>(&level);
        // MC LevelReader.getEffectiveSkyBrightness: max(sky light - skyDarken, 0).
        int target = std::max(level.GetBrightness(Lighting::LightLayer::Sky, pos.x, pos.y, pos.z) -
                              (world ? world->GetSkyDarken() : 0), 0);
        float sunAngle = (world ? TimeOfDay(world->GetDayTime()) : 0.25f) * (2.0f * kPi);
        const bool isInverted = BoolOf(state, PropertyId::INVERTED);
        if (isInverted) {
            target = 15 - target;
        } else if (target > 0) {
            const float offset = sunAngle < kPi ? 0.0f : 2.0f * kPi;
            sunAngle += (offset - sunAngle) * 0.2f;
            target = static_cast<int>(std::lround(static_cast<float>(target) * std::cos(sunAngle)));
        }
        target = std::clamp(target, 0, 15);
        if (PowerOf(state) != target) SetBlockAndUpdate(level, pos, WithPower(state, target));
    }

    // DaylightDetectorBlock.tickEntity: every 20 game ticks, in a dimension
    // with sky light (getTicker: dimensionType().hasSkyLight()).
    void DaylightDetectorBlockEntity::Tick(World* world, float /*deltaTime*/) {
        if (!world || !DimensionHasSkyLight(world->GetDimension())) return;
        if (world->GetGameTime() % 20 != 0) return;
        const glm::ivec3 pos = GetWorldPos();
        const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
        if (!state.Is(BlockID::DaylightDetector)) return;
        DaylightDetectorUpdateSignalStrength(*world, pos, state);
    }


    void RedstoneFlushDeferredChecks(ILevelWrite& level) {
        auto it = s_deferred.find(&level);
        if (it == s_deferred.end()) return;
        // A re-check may defer again (a lamp turning on notifies nothing, a
        // schedule notifies nothing; but keep the loop honest).
        for (int round = 0; round < 8 && !it->second.list.empty(); ++round) {
            std::vector<glm::ivec3> list;
            list.swap(it->second.list);
            it->second.seen.clear();
            for (const glm::ivec3& pos : list) {
                // Same guarantee as the scheduled ticks (World's tick check):
                // no decision next to a chunk that is not resident yet — the
                // settled state it would read is not the real one. Kept
                // listed; re-checked at a later flush once the neighbours are
                // in (the border settle of the arriving chunk re-notifies).
                const int cx = pos.x >> 4, cz = pos.z >> 4;
                bool surrounded = true;
                for (int dz = -1; dz <= 1 && surrounded; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (!level.IsChunkLoaded(cx + dx, cz + dz)) { surrounded = false; break; }
                if (!surrounded) { Defer(level, pos); continue; }
                const BlockState state = StateAt(level, pos);
                const BlockID id = state.Block();
                if (id == BlockID::RedstoneTorch || id == BlockID::RedstoneWallTorch ||
                    id == BlockID::BlueRedstoneTorch || id == BlockID::BlueRedstoneWallTorch) {
                    TorchCheck(level, pos, state);
                } else if (IsDiodeBlock(id)) {
                    if (DiodeCanSurviveOn(level, Below(pos))) DiodeCheckTickOnNeighbor(level, pos, state);
                } else if (id == BlockID::RedstoneLamp) {
                    LampCheck(level, pos, state);
                } else if (id == BlockID::DisplayBlock) {
                    DisplayEvaluate(level, pos);
                }
            }
        }
    }

    bool HasRedstoneSurvivalRule(BlockID id) {
        return id == BlockID::RedstoneTorch || id == BlockID::RedstoneWallTorch ||
               id == BlockID::BlueRedstoneTorch || id == BlockID::BlueRedstoneWallTorch ||
               IsDiodeBlock(id) || IsPressurePlateBlock(id) || id == BlockID::TripwireHook ||
               id == BlockID::Tripwire || id == BlockID::PistonHead;
    }

    bool RedstoneComponentCanSurvive(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
        const BlockID id = state.Block();
        if (id == BlockID::RedstoneWallTorch || id == BlockID::BlueRedstoneWallTorch) return WallTorchCanSurvive(level, pos, HorizontalFacingOf(state));
        if (id == BlockID::RedstoneTorch || id == BlockID::BlueRedstoneTorch)         return CanSupportCenter(level, Below(pos));
        if (IsDiodeBlock(id))                 return DiodeCanSurviveOn(level, Below(pos));
        if (IsPressurePlateBlock(id))         return PlateCanSurvive(level, pos);
        if (id == BlockID::TripwireHook)      return HookCanSurvive(level, pos, state);
        if (id == BlockID::Tripwire)          return true;
        if (id == BlockID::PistonHead)        return PistonHeadCanSurvive(level, pos, state);
        return true;
    }

    // ── Registration ───────────────────────────────────────────────────────

    void RegisterRedstoneBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        auto at = [&blocks](BlockID id) -> Block& { return blocks[static_cast<size_t>(id)]; };

        // Redstone dust.
        {
            Block& wire = at(BlockID::RedstoneWire);
            wire.isSignalSource                = true;
            wire.getSignal                     = &RedstoneWireGetSignal;
            wire.getDirectSignal               = &RedstoneWireGetDirectSignal;
            wire.onPlace                       = &RedstoneWireOnPlace;
            wire.affectNeighborsAfterRemoval   = &RedstoneWireAfterRemoval;
            wire.neighborChanged               = &RedstoneWireNeighborChanged;
            wire.updateShape                   = &RedstoneWireUpdateShapeHook;
            wire.updateIndirectNeighbourShapes = &RedstoneWireUpdateIndirectNeighbourShapes;
        }

        // Torches.
        for (BlockID id : {BlockID::RedstoneTorch, BlockID::RedstoneWallTorch,
                           BlockID::BlueRedstoneTorch, BlockID::BlueRedstoneWallTorch}) {
            Block& b = at(id);
            b.isSignalSource              = true;
            b.onPlace                     = &TorchOnPlace;
            b.affectNeighborsAfterRemoval = &TorchAfterRemoval;
            b.tick                        = &TorchTick;
            b.neighborChanged             = &TorchNeighborChanged;
            b.getSignal                   = &TorchGetSignal;
            b.getDirectSignal             = &TorchGetDirectSignal;
        }
        at(BlockID::RedstoneWallTorch).updateShape = &WallTorchUpdateShape;
        at(BlockID::BlueRedstoneWallTorch).updateShape = &WallTorchUpdateShape;
        at(BlockID::BlueRedstoneTorch).neighborChanged     = &BlueTorchNeighborChanged;
        at(BlockID::BlueRedstoneWallTorch).neighborChanged = &BlueTorchNeighborChanged;

        // Repeater and comparator.
        for (BlockID id : {BlockID::Repeater, BlockID::Comparator}) {
            Block& b = at(id);
            b.isSignalSource              = true;
            b.tick                        = &DiodeTick;
            b.neighborChanged             = &DiodeNeighborChanged;
            b.onPlace                     = &DiodeOnPlace;
            b.affectNeighborsAfterRemoval = &DiodeAfterRemoval;
            b.updateShape                 = &DiodeUpdateShape;
            b.getSignal                   = &DiodeGetSignal;
            b.getDirectSignal             = &DiodeGetSignal;
        }
        at(BlockID::Repeater).useWithoutItem   = &RepeaterUse;
        at(BlockID::Comparator).useWithoutItem = &ComparatorUse;

        // Observer.
        {
            Block& b = at(BlockID::Observer);
            b.isSignalSource              = true;
            b.tick                        = &ObserverTick;
            b.updateShape                 = &ObserverUpdateShape;
            b.getSignal                   = &ObserverGetSignal;
            b.getDirectSignal             = &ObserverGetSignal;
            b.onPlace                     = &ObserverOnPlace;
            b.affectNeighborsAfterRemoval = &ObserverAfterRemoval;
        }

        // Lamp and block of redstone.
        at(BlockID::RedstoneLamp).neighborChanged = &LampNeighborChanged;
        at(BlockID::RedstoneLamp).tick            = &LampTick;
        {
            Block& b = at(BlockID::DisplayBlock);
            b.neighborChanged       = &DisplayNeighborChanged;
            b.onPlace               = &DisplayOnPlace;
            b.getSignal             = &DisplayGetSignal;
            b.getDirectSignal       = &DisplayGetSignal;
            b.getAnalogOutputSignal = &DisplayAnalogOutput;
            b.isSignalSource        = true;
            b.hasAnalogOutputSignal = true;
            b.useWithoutItem        = &DisplayUse;
            b.emissive              = true;
            // Not a conductor: a full cube would relay strong power from a line
            // beside it into the next pixel's faces (the sims treat it as inert).
            b.redstoneConductor     = RedstoneConductor::Never;
        }
        at(BlockID::RedstoneBlock).isSignalSource = true;
        at(BlockID::RedstoneBlock).getSignal      = &RedstoneBlockGetSignal;

        // Lever.
        {
            Block& b = at(BlockID::Lever);
            b.isSignalSource              = true;
            b.useWithoutItem              = &LeverUse;
            b.affectNeighborsAfterRemoval = &FaceAttachedAfterRemoval;
            b.getSignal                   = &FaceAttachedGetSignal;
            b.getDirectSignal             = &FaceAttachedGetDirectSignal;
        }

        // Buttons, plates, doors, trapdoors, gates, bulbs: by family.
        for (size_t i = 0; i < blocks.size(); ++i) {
            const BlockID id = static_cast<BlockID>(i);
            Block& b = blocks[i];
            if (IsButtonBlock(id)) {
                b.isSignalSource              = true;
                b.useWithoutItem              = &ButtonUse;
                b.tick                        = &ButtonTick;
                b.entityInside                = &ButtonEntityInside;
                b.affectNeighborsAfterRemoval = &FaceAttachedAfterRemoval;
                b.getSignal                   = &FaceAttachedGetSignal;
                b.getDirectSignal             = &FaceAttachedGetDirectSignal;
            }
            if (IsPressurePlateBlock(id)) {
                b.isSignalSource              = true;
                b.tick                        = &PlateTick;
                b.entityInside                = &PlateEntityInside;
                b.anyInside                   = &PlateAnyInside;
                b.affectNeighborsAfterRemoval = &PlateAfterRemoval;
                b.getSignal                   = &PlateGetSignal;
                b.getDirectSignal             = &PlateGetDirectSignal;
                b.updateShape                 = &PlateUpdateShape;
            }
            if (IsDoorBlock(id))      b.neighborChanged = &DoorNeighborChanged;
            if (IsTrapdoorBlock(id))  b.neighborChanged = &TrapdoorNeighborChanged;
            if (IsFenceGateBlock(id)) b.neighborChanged = &FenceGateNeighborChanged;
            if (b.registrySlug.find("copper_bulb") != std::string::npos) {
                b.onPlace               = &CopperBulbOnPlace;
                b.neighborChanged       = &CopperBulbNeighborChanged;
                b.hasAnalogOutputSignal = true;
                b.getAnalogOutputSignal = &CopperBulbAnalogOutput;
            }
        }

        // Note block.
        {
            Block& b = at(BlockID::NoteBlock);
            b.updateShape     = &NoteBlockUpdateShape;
            b.neighborChanged = &NoteBlockNeighborChanged;
            b.useWithoutItem  = &NoteBlockUse;
            b.attack          = &NoteBlockAttack;
            b.triggerEvent    = &NoteBlockTriggerEvent;
        }

        // Target.
        {
            Block& b = at(BlockID::Target);
            b.isSignalSource  = true;
            b.onProjectileHit = &TargetOnProjectileHit;
            b.tick            = &TargetTick;
            b.getSignal       = &TargetGetSignal;
            b.onPlace         = &TargetOnPlace;
        }

        // Daylight detector.
        {
            Block& b = at(BlockID::DaylightDetector);
            b.isSignalSource = true;
            b.useWithoutItem = &DaylightDetectorUse;
            b.getSignal      = &DaylightDetectorGetSignal;
        }

        // Redstone ore.
        for (BlockID id : {BlockID::RedstoneOre, BlockID::DeepslateRedstoneOre}) {
            Block& b = at(id);
            b.attack            = &RedstoneOreAttack;
            b.useItemOn         = &RedstoneOreUseItemOn;
            b.isRandomlyTicking = &RedstoneOreIsRandomlyTicking;
            b.randomTick        = &RedstoneOreRandomTick;
        }

        // TNT: the writable neighborChanged replaces the old read-only seam.
        at(BlockID::Tnt).neighborChanged = &TntNeighborChanged;
        at(BlockID::Tnt).updateShape     = nullptr;

        // Tripwire and hook.
        {
            Block& wire = at(BlockID::Tripwire);
            wire.updateShape                 = &TripWireUpdateShape;
            wire.onPlace                     = &TripWireOnPlace;
            wire.affectNeighborsAfterRemoval = &TripWireAfterRemoval;
            wire.entityInside                = &TripWireEntityInside;
            wire.anyInside                   = &TripWireAnyInside;
            wire.tick                        = &TripWireTick;

            Block& hook = at(BlockID::TripwireHook);
            hook.isSignalSource              = true;
            hook.tick                        = &HookTick;
            hook.affectNeighborsAfterRemoval = &HookAfterRemoval;
            hook.getSignal                   = &HookGetSignal;
            hook.getDirectSignal             = &HookGetDirectSignal;
            hook.updateShape                 = &HookUpdateShape;
        }

        Log::Info("[Redstone] component behaviours registered");
    }

} // namespace Game
