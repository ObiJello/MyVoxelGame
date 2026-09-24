// File: src/common/world/block/entity/PistonMovingBlockEntity.hpp
//
// MC PistonMovingBlockEntity: the state a cell carries while a piston is
// moving it — which block is in transit, which way, whether the piston is
// extending, and how far along the two-tick animation it is.
//
// One entity per moved cell plus one for the head (isSourcePiston), created
// by PistonBaseBlock.moveBlocks and retired by its own tick two ticks later,
// at which point the carried block is written for real.
#pragma once

#include "BlockEntity.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"

namespace Game::Nbt { class Writer; }

namespace Game {

    class ILevelWrite;

    class PistonMovingBlockEntity : public BlockEntity {
    public:
        static constexpr int    kTicksToExtend = 2;
        static constexpr double kPushOffset    = 0.01;
        static constexpr double kTickMovement  = 0.51;

        PistonMovingBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        // The six-argument vanilla constructor, as a setter: the factory
        // makes the bare entity and the piston fills it in.
        void Init(BlockState movedState, Direction direction, bool extending, bool isSourcePiston) {
            m_movedState     = movedState;
            m_direction      = direction;
            m_extending      = extending;
            m_isSourcePiston = isSourcePiston;
            m_progress       = 0.0f;
            m_progressO      = 0.0f;
        }

        bool       IsExtending()    const { return m_extending; }
        Direction  GetDirection()   const { return m_direction; }
        bool       IsSourcePiston() const { return m_isSourcePiston; }
        BlockState GetMovedState()  const { return m_movedState; }
        int64_t    GetLastTicked()  const { return m_lastTicked; }
        float      Progress()       const { return m_progress; }

        // MC getProgress(partial): lerp between last tick's value and this one's.
        float GetProgress(float a) const;

        // ── Client-only hand-off ─────────────────────────────────────────
        //
        // When the carried block lands, the client's section mesh that will
        // show it is rebuilt asynchronously. Vanilla drops the entity at once
        // and lives with the gap; here the entity stays "landed" — drawn at
        // rest, never ticked — until the mesh whose version includes the
        // final block has been uploaded (ClientChunkManager retires it).
        void     Land(uint32_t sectionVersion, int64_t clientTick) {
            m_landed = true; m_landedVersion = sectionVersion; m_landedTick = clientTick;
            m_progress = 1.0f; m_progressO = 1.0f;
        }
        bool     IsLanded()      const { return m_landed; }
        uint32_t LandedVersion() const { return m_landedVersion; }
        int64_t  LandedTick()    const { return m_landedTick; }
        float GetXOff(float a) const { return static_cast<float>(StepX(m_direction)) * GetExtendedProgress(GetProgress(a)); }
        float GetYOff(float a) const { return static_cast<float>(StepY(m_direction)) * GetExtendedProgress(GetProgress(a)); }
        float GetZOff(float a) const { return static_cast<float>(StepZ(m_direction)) * GetExtendedProgress(GetProgress(a)); }
        float GetExtendedProgress(float progress) const { return m_extending ? progress - 1.0f : 1.0f - progress; }

        Direction GetMovementDirection() const { return m_extending ? m_direction : Opposite(m_direction); }
        Direction GetPushDirection()     const { return m_extending ? m_direction : Opposite(m_direction); }

        bool NeedsTicking() const override { return true; }
        void Tick(World* world, float deltaTime) override;
        // The same tick on the client's own level (deathTicks branch).
        void ClientTick(ILevelWrite& level) override { Tick(level); }
        // MC PistonMovingBlockEntity.tick(level, pos, state, entity).
        void Tick(ILevelWrite& level);

        // MC finalTick: the move ends now, whatever the progress.
        void FinalTick(ILevelWrite& level);

        // MC preRemoveSideEffects → finalTick.
        void PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos, BlockState oldState) override;

        // Wire form (server → client), and the Anvil form.
        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;
        void WriteNbt(Nbt::Writer& w) const;
        void SetFromNbt(BlockState movedState, Direction direction, float progress,
                        bool extending, bool source) {
            m_movedState = movedState; m_direction = direction;
            m_progress = progress; m_progressO = progress;
            m_extending = extending; m_isSourcePiston = source;
        }

    private:
        // MC getCollisionRelatedBlockState.
        BlockState GetCollisionRelatedBlockState() const;
        static void MoveCollidedEntities(ILevelWrite& level, const glm::ivec3& pos, float newProgress,
                                         PistonMovingBlockEntity& self);
        static void MoveStuckEntities(ILevelWrite& level, const glm::ivec3& pos, float newProgress,
                                      PistonMovingBlockEntity& self);

        BlockState m_movedState{};
        Direction  m_direction      = Direction::Down;
        bool       m_extending      = false;
        bool       m_isSourcePiston = false;
        float      m_progress       = 0.0f;
        float      m_progressO      = 0.0f;
        int64_t    m_lastTicked     = 0;
        int        m_deathTicks     = 0;
        bool       m_landed         = false;
        uint32_t   m_landedVersion  = 0;
        int64_t    m_landedTick     = 0;
    };

} // namespace Game
