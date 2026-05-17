// File: src/common/network/PacketTypes.hpp
//
// UMBRELLA HEADER — pulls in every per-packet definition.
//
// Historically this file was a 1160-line monolith with all 28 packet structs
// and their Serialize/Deserialize functions inline. We split it MC-style:
// one file per packet under `packets/`, organized by protocol phase
// (mirroring net.minecraft.network.protocol.{game,login,handshake,...}).
// Existing consumers can keep `#include "common/network/PacketTypes.hpp"`
// and get the same set of types — this header just re-exports the new
// per-packet headers.
//
// New code should prefer including the specific per-packet header it needs
// (faster compile, clearer dependency graph). Look in:
//   src/common/network/packets/common/    — shared enums/sentinels/worker results
//   src/common/network/packets/game/      — gameplay-phase packets
//   (handshake/login packets remain at packets/{Name}.hpp for now)

#pragma once

// Cross-packet shared types
#include "packets/common/PacketCommon.hpp"   // BlockActionType, ContainerInput, Relative, sentinels
#include "packets/common/WorkerResults.hpp"  // ChunkGenResult, MeshBuildResult, SerializedChunkData

// Server → Client (game phase)
#include "packets/game/ChunkDataS2CPacket.hpp"
#include "packets/game/UnloadChunkS2CPacket.hpp"
#include "packets/game/ChunkBatchStartS2CPacket.hpp"
#include "packets/game/ChunkBatchFinishedS2CPacket.hpp"
#include "packets/game/SetChunkCacheRadiusS2CPacket.hpp"
#include "packets/game/BlockChangeS2CPacket.hpp"
#include "packets/game/ClientboundBlockUpdateS2CPacket.hpp"
#include "packets/game/MultiBlockChangeS2CPacket.hpp"
#include "packets/game/ClientboundSectionBlocksUpdateS2CPacket.hpp"
#include "packets/game/HotbarSyncS2CPacket.hpp"
#include "packets/game/PlayerUpdateS2CPacket.hpp"
#include "packets/game/RemoveEntitiesS2CPacket.hpp"
#include "packets/game/PlayerInfoS2CPacket.hpp"
#include "packets/game/ClientboundPlayerPositionPacket.hpp"
#include "packets/game/InventoryFullS2CPacket.hpp"
#include "packets/game/InventorySetSlotS2CPacket.hpp"
#include "packets/game/InventorySetCarriedS2CPacket.hpp"
#include "packets/game/PortalSetS2CPacket.hpp"            // Features.hpp ENABLE_PORTAL_GUN strips body
#include "packets/game/PortalRemoveS2CPacket.hpp"         // Features.hpp ENABLE_PORTAL_GUN strips body
#include "packets/game/PortalTeleportFlashS2CPacket.hpp"  // Features.hpp ENABLE_PORTAL_GUN strips body
#include "packets/game/PortalFizzleS2CPacket.hpp"         // Features.hpp ENABLE_PORTAL_GUN strips body

// Client → Server (game phase)
#include "packets/game/ChunkBatchAckC2SPacket.hpp"
#include "packets/game/BlockActionC2SPacket.hpp"
#include "packets/game/UseItemOnC2SPacket.hpp"
#include "packets/game/PlayerMoveC2SPacket.hpp"
#include "packets/game/HeldItemChangeC2SPacket.hpp"
#include "packets/game/ServerboundAcceptTeleportationPacket.hpp"
#include "packets/game/InventoryClickC2SPacket.hpp"
#include "packets/game/InventoryCloseC2SPacket.hpp"
#include "packets/game/ChatMessageC2SPacket.hpp"
#include "packets/game/ClientConfigC2SPacket.hpp"

#include "PacketRegistry.hpp"

namespace Network {

    // ── Packet utility templates / stats ───────────────────────────────────
    // Kept here (rather than per-packet) because they're cross-cutting
    // utilities, not packet definitions.

    template<typename PacketType>
    size_t GetPacketSize(const PacketType& packet);

    template<typename PacketType>
    bool ValidatePacket(const PacketType& packet);

    struct PacketStats {
        size_t serverToClientCount   = 0;
        size_t clientToServerCount   = 0;
        size_t chunkGenResultCount   = 0;
        size_t meshBuildResultCount  = 0;
        size_t totalBytesTransferred = 0;

        void Reset() {
            serverToClientCount = clientToServerCount = 0;
            chunkGenResultCount = meshBuildResultCount = 0;
            totalBytesTransferred = 0;
        }
    };

    namespace Serialization {

        // Helper function template to serialize any packet by ADL — the
        // matching `Serialize(const T&)` overload from each per-packet
        // header is found by argument-dependent lookup.
        template<typename PacketType>
        inline std::vector<uint8_t> SerializePacket(const PacketType& packet) {
            return Serialize(packet);
        }

    } // namespace Serialization

} // namespace Network
