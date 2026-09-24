// File: src/common/world/block/Rails.cpp
#include "common/world/block/Rails.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/RedstoneFamilies.hpp"
#include "common/world/block/RedstoneSignal.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"

#include <optional>
#include <vector>

namespace Game {

    namespace {

        PropertyId ShapeProp(BlockID id) {
            return id == BlockID::Rail ? PropertyId::RAIL_SHAPE : PropertyId::RAIL_SHAPE_STRAIGHT;
        }

        BlockState StateAt(const IBlockAccess& level, const glm::ivec3& p) {
            return level.GetBlockState(p.x, p.y, p.z);
        }

        bool IsRail(const IBlockAccess& level, const glm::ivec3& p) {
            return IsRailBlock(level.GetBlock(p.x, p.y, p.z));
        }

        void SetBlockAndUpdate(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            level.SetBlock(pos.x, pos.y, pos.z, state, World::UpdateFlags::All);
        }

        // MC Block.canSupportRigidBlock(level, pos): a full sturdy top.
        bool CanSupportRigidBlock(const IBlockAccess& level, const glm::ivec3& pos) {
            return IsFaceSturdyAt(level, pos, Direction::Up);
        }

        // ── RailState ────────────────────────────────────────────────────

        class RailState {
        public:
            RailState(ILevelWrite& level, const glm::ivec3& pos, BlockState state)
                : m_level(level), m_pos(pos), m_state(state),
                  m_block(state.Block()), m_isStraight(IsStraightRail(state.Block())) {
                UpdateConnections(RailShapeOf(state));
            }

            const std::vector<glm::ivec3>& GetConnections() const { return m_connections; }
            BlockState GetState() const { return m_state; }
            const glm::ivec3& Pos() const { return m_pos; }

            int CountPotentialConnections() const {
                int count = 0;
                for (Direction d : kHorizontalPlane) {
                    if (HasRail(Relative(m_pos, d))) ++count;
                }
                return count;
            }

            RailState& Place(bool hasSignal, bool first, RailShape defaultShape) {
                const glm::ivec3 north = Relative(m_pos, Direction::North);
                const glm::ivec3 south = Relative(m_pos, Direction::South);
                const glm::ivec3 west  = Relative(m_pos, Direction::West);
                const glm::ivec3 east  = Relative(m_pos, Direction::East);
                const bool n = HasNeighborRail(north);
                const bool s = HasNeighborRail(south);
                const bool w = HasNeighborRail(west);
                const bool e = HasNeighborRail(east);
                std::optional<RailShape> shape;
                const bool northOrSouth = n || s;
                const bool westOrEast   = w || e;
                if (northOrSouth && !westOrEast) shape = RailShape::NorthSouth;
                if (westOrEast && !northOrSouth) shape = RailShape::EastWest;
                const bool southAndEast = s && e;
                const bool southAndWest = s && w;
                const bool northAndEast = n && e;
                const bool northAndWest = n && w;
                if (!m_isStraight) {
                    if (southAndEast && !n && !w) shape = RailShape::SouthEast;
                    if (southAndWest && !n && !e) shape = RailShape::SouthWest;
                    if (northAndWest && !s && !e) shape = RailShape::NorthWest;
                    if (northAndEast && !s && !w) shape = RailShape::NorthEast;
                }
                if (!shape) {
                    if (northOrSouth && westOrEast) shape = defaultShape;
                    else if (northOrSouth)          shape = RailShape::NorthSouth;
                    else if (westOrEast)            shape = RailShape::EastWest;
                    if (!m_isStraight) {
                        if (hasSignal) {
                            if (southAndEast) shape = RailShape::SouthEast;
                            if (southAndWest) shape = RailShape::SouthWest;
                            if (northAndEast) shape = RailShape::NorthEast;
                            if (northAndWest) shape = RailShape::NorthWest;
                        } else {
                            if (northAndWest) shape = RailShape::NorthWest;
                            if (northAndEast) shape = RailShape::NorthEast;
                            if (southAndWest) shape = RailShape::SouthWest;
                            if (southAndEast) shape = RailShape::SouthEast;
                        }
                    }
                }
                if (shape == RailShape::NorthSouth) {
                    if (IsRail(m_level, Above(north))) shape = RailShape::AscendingNorth;
                    if (IsRail(m_level, Above(south))) shape = RailShape::AscendingSouth;
                }
                if (shape == RailShape::EastWest) {
                    if (IsRail(m_level, Above(east))) shape = RailShape::AscendingEast;
                    if (IsRail(m_level, Above(west))) shape = RailShape::AscendingWest;
                }
                if (!shape) shape = defaultShape;

                UpdateConnections(*shape);
                m_state = WithRailShape(m_state, *shape);
                if (first || StateAt(m_level, m_pos) != m_state) {
                    SetBlockAndUpdate(m_level, m_pos, m_state);
                    for (size_t i = 0; i < m_connections.size(); ++i) {
                        std::optional<RailState> neighbor = GetRail(m_connections[i]);
                        if (neighbor) {
                            neighbor->RemoveSoftConnections();
                            if (neighbor->CanConnectTo(*this)) neighbor->ConnectTo(*this);
                        }
                    }
                }
                return *this;
            }

        private:
            void UpdateConnections(RailShape direction) {
                m_connections.clear();
                const glm::ivec3& p = m_pos;
                auto N = Relative(p, Direction::North), S = Relative(p, Direction::South);
                auto W = Relative(p, Direction::West),  E = Relative(p, Direction::East);
                switch (direction) {
                    case RailShape::NorthSouth:     m_connections = {N, S}; break;
                    case RailShape::EastWest:       m_connections = {W, E}; break;
                    case RailShape::AscendingEast:  m_connections = {W, Above(E)}; break;
                    case RailShape::AscendingWest:  m_connections = {Above(W), E}; break;
                    case RailShape::AscendingNorth: m_connections = {Above(N), S}; break;
                    case RailShape::AscendingSouth: m_connections = {N, Above(S)}; break;
                    case RailShape::SouthEast:      m_connections = {E, S}; break;
                    case RailShape::SouthWest:      m_connections = {W, S}; break;
                    case RailShape::NorthWest:      m_connections = {W, N}; break;
                    case RailShape::NorthEast:      m_connections = {E, N}; break;
                }
            }

            void RemoveSoftConnections() {
                for (size_t i = 0; i < m_connections.size(); ++i) {
                    std::optional<RailState> rail = GetRail(m_connections[i]);
                    if (rail && rail->ConnectsTo(*this)) {
                        m_connections[i] = rail->m_pos;
                    } else {
                        m_connections.erase(m_connections.begin() + static_cast<long>(i));
                        --i;
                    }
                }
            }

            bool HasRail(const glm::ivec3& pos) const {
                return IsRail(m_level, pos) || IsRail(m_level, Above(pos)) || IsRail(m_level, Below(pos));
            }

            std::optional<RailState> GetRail(const glm::ivec3& pos) const {
                for (const glm::ivec3& testPos : {pos, Above(pos), Below(pos)}) {
                    const BlockState testState = StateAt(m_level, testPos);
                    if (IsRailBlock(testState.Block())) return RailState(m_level, testPos, testState);
                }
                return std::nullopt;
            }

            bool ConnectsTo(const RailState& rail) const { return HasConnection(rail.m_pos); }

            bool HasConnection(const glm::ivec3& railPos) const {
                for (const glm::ivec3& pos : m_connections) {
                    if (pos.x == railPos.x && pos.z == railPos.z) return true;
                }
                return false;
            }

            bool CanConnectTo(const RailState& rail) const {
                return ConnectsTo(rail) || m_connections.size() != 2;
            }

            void ConnectTo(const RailState& rail) {
                m_connections.push_back(rail.m_pos);
                const glm::ivec3 north = Relative(m_pos, Direction::North);
                const glm::ivec3 south = Relative(m_pos, Direction::South);
                const glm::ivec3 west  = Relative(m_pos, Direction::West);
                const glm::ivec3 east  = Relative(m_pos, Direction::East);
                const bool n = HasConnection(north), s = HasConnection(south);
                const bool w = HasConnection(west),  e = HasConnection(east);
                std::optional<RailShape> shape;
                if (n || s) shape = RailShape::NorthSouth;
                if (w || e) shape = RailShape::EastWest;
                if (!m_isStraight) {
                    if (s && e && !n && !w) shape = RailShape::SouthEast;
                    if (s && w && !n && !e) shape = RailShape::SouthWest;
                    if (n && w && !s && !e) shape = RailShape::NorthWest;
                    if (n && e && !s && !w) shape = RailShape::NorthEast;
                }
                if (shape == RailShape::NorthSouth) {
                    if (IsRail(m_level, Above(north))) shape = RailShape::AscendingNorth;
                    if (IsRail(m_level, Above(south))) shape = RailShape::AscendingSouth;
                }
                if (shape == RailShape::EastWest) {
                    if (IsRail(m_level, Above(east))) shape = RailShape::AscendingEast;
                    if (IsRail(m_level, Above(west))) shape = RailShape::AscendingWest;
                }
                if (!shape) shape = RailShape::NorthSouth;
                m_state = WithRailShape(m_state, *shape);
                SetBlockAndUpdate(m_level, m_pos, m_state);
            }

            bool HasNeighborRail(const glm::ivec3& railPos) const {
                std::optional<RailState> neighbor = GetRail(railPos);
                if (!neighbor) return false;
                neighbor->RemoveSoftConnections();
                return neighbor->CanConnectTo(*this);
            }

            ILevelWrite& m_level;
            glm::ivec3   m_pos;
            BlockState   m_state;
            BlockID      m_block;
            bool         m_isStraight;
            std::vector<glm::ivec3> m_connections;
        };

        // ── BaseRailBlock ────────────────────────────────────────────────

        BlockState UpdateDir(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool first) {
            if (level.IsClientSide()) return state;
            const RailShape current = RailShapeOf(state);
            return RailState(level, pos, state).Place(HasNeighborSignal(level, pos), first, current).GetState();
        }

        bool ShouldBeRemoved(const glm::ivec3& pos, const IBlockAccess& level, RailShape shape) {
            if (!CanSupportRigidBlock(level, Below(pos))) return true;
            switch (shape) {
                case RailShape::AscendingEast:  return !CanSupportRigidBlock(level, Relative(pos, Direction::East));
                case RailShape::AscendingWest:  return !CanSupportRigidBlock(level, Relative(pos, Direction::West));
                case RailShape::AscendingNorth: return !CanSupportRigidBlock(level, Relative(pos, Direction::North));
                case RailShape::AscendingSouth: return !CanSupportRigidBlock(level, Relative(pos, Direction::South));
                default: return false;
            }
        }

        // ── PoweredRailBlock ─────────────────────────────────────────────

        bool IsSameRailWithPower(ILevelWrite& level, const glm::ivec3& pos, bool forward, int searchDepth,
                                 RailShape dir);

        bool FindPoweredRailSignal(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                   bool forward, int searchDepth) {
            if (searchDepth >= 8) return false;
            int x = pos.x, y = pos.y, z = pos.z;
            bool checkBelow = true;
            RailShape shape = RailShapeOf(state);
            switch (shape) {
                case RailShape::NorthSouth:
                    if (forward) ++z; else --z;
                    break;
                case RailShape::EastWest:
                    if (forward) --x; else ++x;
                    break;
                case RailShape::AscendingEast:
                    if (forward) { --x; } else { ++x; ++y; checkBelow = false; }
                    shape = RailShape::EastWest;
                    break;
                case RailShape::AscendingWest:
                    if (forward) { --x; ++y; checkBelow = false; } else { ++x; }
                    shape = RailShape::EastWest;
                    break;
                case RailShape::AscendingNorth:
                    if (forward) { ++z; } else { --z; ++y; checkBelow = false; }
                    shape = RailShape::NorthSouth;
                    break;
                case RailShape::AscendingSouth:
                    if (forward) { ++z; ++y; checkBelow = false; } else { --z; }
                    shape = RailShape::NorthSouth;
                    break;
                default: break;
            }
            if (IsSameRailWithPower(level, glm::ivec3(x, y, z), forward, searchDepth, shape)) return true;
            return checkBelow && IsSameRailWithPower(level, glm::ivec3(x, y - 1, z), forward, searchDepth, shape);
        }

        bool IsSameRailWithPower(ILevelWrite& level, const glm::ivec3& pos, bool forward, int searchDepth,
                                 RailShape dir) {
            const BlockState state = StateAt(level, pos);
            if (!state.Is(BlockID::PoweredRail)) return false;
            const RailShape myShape = RailShapeOf(state);
            if (dir == RailShape::EastWest &&
                (myShape == RailShape::NorthSouth || myShape == RailShape::AscendingNorth ||
                 myShape == RailShape::AscendingSouth)) {
                return false;
            }
            if (dir == RailShape::NorthSouth &&
                (myShape == RailShape::EastWest || myShape == RailShape::AscendingEast ||
                 myShape == RailShape::AscendingWest)) {
                return false;
            }
            if (!PoweredOf(state)) return false;
            return HasNeighborSignal(level, pos) ? true
                 : FindPoweredRailSignal(level, pos, state, forward, searchDepth + 1);
        }

        // MC RailBlock / PoweredRailBlock / DetectorRailBlock.updateState(state, level, pos, block).
        void UpdateStateForNeighbor(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockID block) {
            const BlockID id = state.Block();
            if (id == BlockID::Rail) {
                if (BlockRegistry::Get(block).isSignalSource &&
                    RailState(level, pos, state).CountPotentialConnections() == 3) {
                    UpdateDir(level, pos, state, false);
                }
                return;
            }
            if (id == BlockID::PoweredRail) {
                const bool isPowered = PoweredOf(state);
                const bool shouldPower = HasNeighborSignal(level, pos) ||
                                         FindPoweredRailSignal(level, pos, state, true, 0) ||
                                         FindPoweredRailSignal(level, pos, state, false, 0);
                if (shouldPower != isPowered) {
                    SetBlockAndUpdate(level, pos, WithPowered(state, shouldPower));
                    level.UpdateNeighborsAt(Below(pos), id);
                    if (RailShapeIsSlope(RailShapeOf(state))) level.UpdateNeighborsAt(Above(pos), id);
                }
                return;
            }
            // Activator rail: shape only. Detector rail: no minecarts to detect.
        }

        // BaseRailBlock.updateState(state, level, pos, movedByPiston) — the
        // placement-time one.
        BlockState UpdateStateOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool movedByPiston) {
            state = UpdateDir(level, pos, state, true);
            if (IsStraightRail(state.Block())) {
                if (auto* world = dynamic_cast<World*>(&level)) {
                    world->NeighborChanged(state, pos, state.Block(), movedByPiston);
                }
            }
            return state;
        }

        void RailOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockState oldState,
                         bool movedByPiston) {
            if (oldState.Block() != state.Block()) UpdateStateOnPlace(level, pos, state, movedByPiston);
        }

        void RailNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state, BlockID block,
                                 bool movedByPiston) {
            if (level.IsClientSide() || !StateAt(level, pos).Is(state.Block())) return;
            if (ShouldBeRemoved(pos, level, RailShapeOf(state))) {
                DropBlockLoot(level, pos, state);
                if (auto* world = dynamic_cast<World*>(&level)) world->RemoveBlock(pos, movedByPiston);
                else level.SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);
            } else {
                UpdateStateForNeighbor(level, pos, state, block);
            }
        }

        void RailAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool movedByPiston) {
            if (movedByPiston) return;
            if (RailShapeIsSlope(RailShapeOf(state))) level.UpdateNeighborsAt(Above(pos), state.Block());
            if (IsStraightRail(state.Block())) {
                level.UpdateNeighborsAt(pos, state.Block());
                level.UpdateNeighborsAt(Below(pos), state.Block());
            }
        }

        // Detector rail signal (never powered without minecarts, but the
        // hooks exist so a track reading is complete).
        int DetectorGetSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction) {
            return PoweredOf(state) ? 15 : 0;
        }
        int DetectorGetDirectSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction direction) {
            return PoweredOf(state) && direction == Direction::Up ? 15 : 0;
        }

    } // namespace

    RailShape RailShapeOf(BlockState state) {
        return static_cast<RailShape>(state.GetIndex(ShapeProp(state.Block())));
    }

    BlockState WithRailShape(BlockState state, RailShape shape) {
        // The straight rails' property has only the first six values; a
        // corner asked of one is refused by SetIndex (range check), which
        // matches vanilla never producing one.
        return state.SetIndex(ShapeProp(state.Block()), static_cast<int>(shape));
    }

    BlockState RailPlacementState(BlockState state, Direction horizontalDirection) {
        const bool isEastWest = horizontalDirection == Direction::East || horizontalDirection == Direction::West;
        return WithRailShape(state, isEastWest ? RailShape::EastWest : RailShape::NorthSouth);
    }

    void RegisterRailBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        for (size_t i = 0; i < blocks.size(); ++i) {
            const BlockID id = static_cast<BlockID>(i);
            if (!IsRailBlock(id)) continue;
            Block& b = blocks[i];
            b.onPlace                     = &RailOnPlace;
            b.neighborChanged             = &RailNeighborChanged;
            b.affectNeighborsAfterRemoval = &RailAfterRemoval;
        }
        Block& detector = blocks[static_cast<size_t>(BlockID::DetectorRail)];
        detector.isSignalSource  = true;
        detector.getSignal       = &DetectorGetSignal;
        detector.getDirectSignal = &DetectorGetDirectSignal;
    }

} // namespace Game
