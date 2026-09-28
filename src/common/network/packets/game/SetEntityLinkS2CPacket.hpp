// File: src/common/network/packets/game/SetEntityLinkS2CPacket.hpp
//
// MC ClientboundSetEntityLinkPacket — a leashed mob and what holds its lead.
//
// Sent by the entity tracker (ServerEntityTracker) to a watcher right after
// the leashed mob's AddEntityS2C (MC ServerEntity.sendPairingData) and to
// every watcher whenever the holder changes (MC setLeashedTo / dropLeash).
// `destId` is the holder's entity id — a player's connection id, a mob's,
// a fence knot's — or Game::Leash::kNoHolder (-1) when the lead came off:
// MC sends 0 for "none", but 0 is a valid player id here.
//
// The holder may not be known to the client yet (its AddEntity can follow);
// the client keeps the id and resolves it when it draws the rope, as MC's
// delayedLeashHolderId does.
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct SetEntityLinkS2CPacket {
        int32_t sourceId = 0;    // the leashed mob
        int32_t destId   = -1;   // the holder, or -1 (Game::Leash::kNoHolder)
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const SetEntityLinkS2CPacket& p) {
            // MC writes both as plain ints.
            PacketBuffer b;
            b.WriteInt(static_cast<uint32_t>(p.sourceId));
            b.WriteInt(static_cast<uint32_t>(p.destId));
            return b.GetData();
        }

        inline SetEntityLinkS2CPacket DeserializeSetEntityLinkS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            SetEntityLinkS2CPacket p;
            p.sourceId = static_cast<int32_t>(r.ReadInt());
            p.destId   = static_cast<int32_t>(r.ReadInt());
            return p;
        }

    } // namespace Serialization

} // namespace Network
