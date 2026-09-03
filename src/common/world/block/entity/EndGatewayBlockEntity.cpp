// File: src/common/world/block/entity/EndGatewayBlockEntity.cpp
#include "EndGatewayBlockEntity.hpp"

#include "common/network/PacketRegistry.hpp"

namespace Game {

    void EndGatewayBlockEntity::Tick(World* /*world*/, float /*deltaTime*/) {
        // MC TheEndGatewayBlockEntity.portalTick. The 2400-tick attention
        // pulse re-arms the cooldown beam so an idle gateway keeps signalling
        // where it is.
        ++m_age;
        if (m_teleportCooldown > 0) {
            --m_teleportCooldown;
        } else if (m_age % 2400 == 0) {
            TriggerCooldown();
        }
    }

    void EndGatewayBlockEntity::Save(Network::PacketBuffer& out) const {
        out.WriteLong(static_cast<uint64_t>(m_age));
        out.WriteByte(m_hasExit ? 1 : 0);
        out.WriteInt(static_cast<uint32_t>(m_exitPortal.x));
        out.WriteInt(static_cast<uint32_t>(m_exitPortal.y));
        out.WriteInt(static_cast<uint32_t>(m_exitPortal.z));
        out.WriteByte(m_exactTeleport ? 1 : 0);
    }

    void EndGatewayBlockEntity::Load(Network::PacketReader& in) {
        m_age = static_cast<int64_t>(in.ReadLong());
        m_hasExit = in.ReadByte() != 0;
        m_exitPortal.x = static_cast<int32_t>(in.ReadInt());
        m_exitPortal.y = static_cast<int32_t>(in.ReadInt());
        m_exitPortal.z = static_cast<int32_t>(in.ReadInt());
        m_exactTeleport = in.ReadByte() != 0;
    }

} // namespace Game
