// File: src/common/world/block/entity/EndGatewayBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.TheEndGatewayBlockEntity —
// the state behind an End gateway: the beam animation clock, the teleport
// cooldown, and the cached far-island exit position.
//
// The teleport ITSELF is server code (Server::PortalTravel's EndGateway
// branch): it needs a ServerLevel to load chunks a thousand blocks out and
// to place the return gateway. This BE only carries state. Worldgen gateways
// (the end_highlands return gateways the terrain library places) never get a
// BE — worldgen writes blocks, not entities — so the travel code creates one
// lazily on the first entry; a gateway placed at runtime (the fight's, or a
// return gateway) gets its BE through World::SetBlock like any other.
#pragma once

#include "BlockEntity.hpp"

namespace Game {

    class EndGatewayBlockEntity : public BlockEntity {
    public:
        // MC TheEndGatewayBlockEntity constants.
        static constexpr int64_t kSpawnTime = 200;      // magenta spawn beam
        static constexpr int     kCooldownTime = 40;    // purple post-teleport beam
        static constexpr int     kGatewayHeightAboveSurface = 10;

        EndGatewayBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos,
                              BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        // MC portalTick / beamAnimationTick: the age climbs forever, the
        // cooldown unwinds. (MC also re-fires the cooldown beam every 2400
        // ticks as an idle "look at me" pulse — kept.)
        bool NeedsTicking() const override { return true; }
        void Tick(World* world, float deltaTime) override;

        bool IsSpawning() const { return m_age < kSpawnTime; }
        bool IsCoolingDown() const { return m_teleportCooldown > 0; }
        int64_t Age() const { return m_age; }
        void SetAge(int64_t age) { m_age = age; }   // the disk-load path

        void TriggerCooldown() {
            m_teleportCooldown = kCooldownTime;
            MarkDirty();
        }

        bool HasExitPosition() const { return m_hasExit; }
        const glm::ivec3& ExitPosition() const { return m_exitPortal; }
        bool ExactTeleport() const { return m_exactTeleport; }
        void SetExitPosition(const glm::ivec3& exit, bool exact) {
            m_exitPortal = exit;
            m_hasExit = true;
            m_exactTeleport = exact;
            MarkDirty();
        }

        // Wire/disk state (MC saves Age / exit_portal / ExactTeleport; the
        // cooldown is runtime-only there too).
        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        int64_t    m_age = 0;
        int        m_teleportCooldown = 0;
        glm::ivec3 m_exitPortal{0};
        bool       m_hasExit = false;
        bool       m_exactTeleport = false;
    };

} // namespace Game
