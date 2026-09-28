// File: src/common/world/block/entity/BellBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.BellBlockEntity — the bell's
// swing and its raid alarm.
//
//   onHit(face)     the server's ring: the swing starts (or restarts) and
//                   block event 1 carries the struck face to every client.
//   triggerEvent 1  both sides: the living things within 48 blocks are
//                   gathered (at most once per 60 ticks), those within 32
//                   hear the bell (HEARD_BELL_TIME — villagers run home), and
//                   the 50-tick swing starts from the struck face.
//   tick            both sides: the swing counts up; 5 ticks in, with a
//                   raider within 32 blocks, the bell resonates (the
//                   resonate sound) and 40 ticks later every raider within
//                   48 glows for 3 s (server) while the client throws the
//                   ENTITY_EFFECT rings towards them (showBellParticles).
//
// BellRenderer draws the body swinging away from `clickDirection` for the
// `ticks` of the swing. Nothing of this is saved (MC saves none).
#pragma once

#include "BlockEntity.hpp"

#include <cstdint>
#include <vector>

namespace Game {

    class BellBlockEntity : public BlockEntity {
    public:
        static constexpr int kDuration = 50;               // MC DURATION
        static constexpr int kGlowDuration = 60;           // MC GLOW_DURATION
        static constexpr int kMinTicksBetweenSearches = 60;
        static constexpr int kMaxResonationTicks = 40;
        static constexpr int kTicksBeforeResonation = 5;
        static constexpr int kSearchRadius = 48;
        static constexpr int kHearBellRadius = 32;
        static constexpr int kHighlightRaidersRadius = 48;

        BellBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        // MC onHit(direction): `face` is a 3D data value (0 down .. 5 east).
        void OnHit(ILevelWrite& level, int face);

        int  Ticks() const { return m_ticks; }
        bool Shaking() const { return m_shaking; }
        int  ClickDirection() const { return m_clickDirection; }

        // ── BlockEntity ───────────────────────────────────────────────────
        bool NeedsTicking() const override { return true; }
        void Tick(World* world, float deltaTime) override;
        void ClientTick(ILevelWrite& level) override;
        bool TriggerEvent(int b0, int b1) override;
        void CarryClientState(const BlockEntity& previous) override;

    private:
        // The shared ticker; `server` picks the resonation's end action.
        void TickCommon(ILevelWrite& level, bool server);
        void UpdateEntities(ILevelWrite& level);
        bool AreRaidersNearby(ILevelWrite& level) const;
        void MakeRaidersGlow(ILevelWrite& level) const;
        void ShowBellParticles(ILevelWrite& level) const;

        int64_t m_lastRingTimestamp = 0;
        int     m_ticks = 0;
        bool    m_shaking = false;
        int     m_clickDirection = 2;   // Direction.NORTH until struck
        bool    m_haveNearby = false;
        std::vector<int32_t> m_nearbyEntities;   // entity ids, MC's nearbyEntities
        bool    m_resonating = false;
        int     m_resonationTicks = 0;
    };

} // namespace Game
