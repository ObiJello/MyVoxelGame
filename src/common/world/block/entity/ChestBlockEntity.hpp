// File: src/common/world/block/entity/ChestBlockEntity.hpp
//
// Per-cell chest state. Stores the cardinal `facing` direction set at
// placement time so the renderer can rotate the chest model to face the
// player who placed it. Mirrors MC's `ChestBlock.FACING` blockstate
// property; in MC the BE reads it back from the blockstate. We store it
// directly on the BE since our blocks are flat.
//
// One class backs Chest, TrappedChest, AND EnderChest — they share their
// renderer in MC (BlockEntityRenderers.java registers ChestRenderer::new for
// all three). The variant is encoded in m_blockId (inherited from
// BlockEntity).
//
// Holds the 27-slot inventory (MC ChestBlockEntity.items). Save/Load of the
// contents comes from BaseContainerBlockEntity.
//
// The lid (MC openersCounter + chestLidController):
//   SERVER  StartOpen/StopOpen count who has the chest open (players with its
//           menu up, copper golems at it). Every change books block event 1
//           carrying the count; RecheckOpen, a scheduled block tick every 5
//           ticks while anyone is counted, recounts from scratch so a player
//           who disconnected or changed dimension cannot leave it open.
//   BOTH    TriggerEvent(1, count) sets the lid's target (open iff count > 0).
//   CLIENT  ClientTick moves the lid 0.1 per tick toward it; ChestRenderer
//           draws GetOpenNess(partialTick).
// Neither the count nor the lid is saved, exactly as in MC.
#pragma once

#include "BaseContainerBlockEntity.hpp"
#include "common/network/PacketRegistry.hpp"

#include <algorithm>

namespace Game {

    class ILevelWrite;

    // MC ChestLidController, verbatim.
    class ChestLidController {
    public:
        void TickLid() {
            m_oOpenness = m_openness;
            if (!m_shouldBeOpen && m_openness > 0.0f) {
                m_openness = std::max(m_openness - 0.1f, 0.0f);
            } else if (m_shouldBeOpen && m_openness < 1.0f) {
                m_openness = std::min(m_openness + 0.1f, 1.0f);
            }
        }
        float GetOpenness(float a) const { return m_oOpenness + a * (m_openness - m_oOpenness); }
        void  ShouldBeOpen(bool shouldBeOpen) { m_shouldBeOpen = shouldBeOpen; }

        // Nothing left to animate: at its target and done interpolating.
        // Not MC — its ticker runs for every chest forever; this lets the
        // client's ticking list drop a chest whose lid is still.
        bool IsAtRest() const {
            const float target = m_shouldBeOpen ? 1.0f : 0.0f;
            return m_openness == target && m_oOpenness == target;
        }

    private:
        bool  m_shouldBeOpen = false;
        float m_openness     = 0.0f;
        float m_oOpenness    = 0.0f;
    };

    // Cardinal direction enum. Matches MC's Direction ordering for
    // horizontal directions; FacingToYRot returns the Y-rotation needed
    // to align the model's +Z (lock side) with this direction.
    enum class HorizontalDirection : uint8_t {
        North = 0,   // -Z
        South = 1,   // +Z
        West  = 2,   // -X
        East  = 3,   // +X
    };

    class ChestBlockEntity : public BaseContainerBlockEntity {
    public:
        // MC ChestBlockEntity is a 27-slot container (a double chest is two of
        // them joined by a CompoundContainer, not one 54-slot BE).
        static constexpr int SLOT_COUNT = 27;

        ChestBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BaseContainerBlockEntity(type, worldPos, blockId, SLOT_COUNT) {}

        // MC ChestBlockEntity.EVENT_SET_OPEN_COUNT.
        static constexpr int kEventSetOpenCount = 1;

        // ── Openers (server) — MC ContainerOpenersCounter ──────────────────
        // A user (a player whose chest menu covers this chest, a copper golem
        // working it) starts or stops having it open. Spectators never count;
        // callers skip them, as MC's startOpen/stopOpen do.
        void StartOpen(ILevelWrite& level);
        void StopOpen(ILevelWrite& level);
        // MC recheckOpeners: recount the users from the world and settle the
        // lid to match. Driven by the chest's scheduled block tick.
        void RecheckOpen(ILevelWrite& level);
        int  GetOpenerCount() const { return m_openCount; }

        // How the server counts users for RecheckOpen: players and copper
        // golems that currently have the chest at `pos` open (MC
        // getEntitiesWithContainerOpen). Installed by the server; common code
        // cannot see sessions. Unset (null), a recheck keeps the running count.
        using UserCounter = int (*)(ILevelWrite& level, const glm::ivec3& pos);
        static void SetUserCounter(UserCounter counter);

        // ── Lid (both sides) — MC ChestBlockEntity.triggerEvent ────────────
        bool TriggerEvent(int b0, int b1) override;
        // MC getOpenNess(partialTick): 0 closed .. 1 open, before the
        // renderer's ease.
        float GetOpenNess(float partialTick) const { return m_lid.GetOpenness(partialTick); }

        // MC lidAnimateTick is the client ticker. Ticking only while the lid
        // moves keeps closed chests off the client's ticking list.
        bool NeedsTicking() const override { return !m_lid.IsAtRest(); }
        void ClientTick(ILevelWrite& /*level*/) override { m_lid.TickLid(); }
        // An update packet builds a new client entity (ClientConnection);
        // the lid's motion is client-only state and carries over, as MC's
        // load-into-the-existing-entity keeps it.
        void CarryClientState(const BlockEntity& previous) override {
            if (const auto* chest = dynamic_cast<const ChestBlockEntity*>(&previous)) m_lid = chest->m_lid;
        }

        // Facing deliberately does NOT live here. In vanilla, orientation is a
        // blockstate property and a chest's block entity carries none — the
        // whole field set is items + openersCounter + chestLidController
        // (ChestBlockEntity.java:33-35), and ChestRenderer reads the angle off
        // the block (ChestRenderer.java:67). We match that: the facing is on the
        // block's state, written at placement by the shared placement table and
        // replicated with the block itself, so it needs no separate BE sync and
        // survives a world reload with the chunk rather than alongside it.
        //
        // Save/Load are BaseContainerBlockEntity's — they write the 27 stacks.
        // The old placeholder byte is gone; overriding them here again would
        // silently drop every chest's contents on save.

    private:
        // MC ContainerOpenersCounter's callbacks for ChestBlockEntity /
        // EnderChestBlockEntity / TrappedChestBlockEntity.
        void OnOpen(ILevelWrite& level);
        void OnClose(ILevelWrite& level);
        void OpenerCountChanged(ILevelWrite& level, int previous, int current);
        void ScheduleRecheck(ILevelWrite& level);
        void PlayLidSound(ILevelWrite& level, bool open);

        int                m_openCount = 0;
        ChestLidController m_lid;
    };

    // Convert a HorizontalDirection to the Y-rotation in radians needed
    // to point the chest's lock (+Z face in the model) along that
    // direction.
    //
    // glm's RH rotation around +Y produces the matrix
    //     [ cosθ  0  sinθ ]
    //     [  0    1   0   ]
    //     [-sinθ  0  cosθ ]
    // Applied to (0,0,1) (the +Z lock direction) it gives (sinθ, 0, cosθ):
    //   θ = 0     → +Z = south
    //   θ = π/2   → +X = east
    //   θ = π     → -Z = north
    //   θ = -π/2  → -X = west
    // Earlier this table had East/West swapped — symptom was "two opposite
    // ways face me, two face away" because π and 0 are symmetric under the
    // sign flip but ±π/2 are not.
    inline float ChestFacingToYRot(HorizontalDirection d) {
        switch (d) {
            case HorizontalDirection::South: return  0.0f;
            case HorizontalDirection::East:  return  1.5707963f;
            case HorizontalDirection::North: return  3.1415927f;
            case HorizontalDirection::West:  return -1.5707963f;
        }
        return 0.0f;
    }

} // namespace Game
