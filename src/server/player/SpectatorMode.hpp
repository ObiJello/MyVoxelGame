// File: src/server/player/SpectatorMode.hpp
//
// The server half of spectator mode that reaches past one ServerPlayer —
// MC ServerPlayer.setGameMode / setCamera / tick's camera block,
// ServerPlayerGameMode's SPECTATOR branches, and the two spectator packets
// (ServerGamePacketListenerImpl.handleSpectatorAction /
// handleTeleportToEntityPacket).
//
// The per-player state (game mode, previous mode, camera id) lives on
// ServerPlayer; everything here needs the session, the level or the other
// players as well. Server thread only.
#pragma once

#include "ServerPlayer.hpp"

#include <glm/glm.hpp>
#include <cstdint>

namespace Game { class World; }
namespace Network {
    struct SpectatorActionC2SPacket;
    struct TeleportToEntityC2SPacket;
}

namespace Server {

    class PlayerSession;

    namespace Spectator {

        // MC ServerPlayer.setGameMode: the change itself (ServerPlayer::
        // setGameMode, with changeGameModeForPlayer's "flying stops near the
        // ground" rule), then its consequences — a spectator stops using its
        // item; anyone else gets their own camera back — the abilities packet
        // to the player and UPDATE_GAME_MODE to everyone. False, and nothing
        // sent, when the player is already in `mode`.
        bool ChangeGameMode(PlayerSession& session, GameMode mode);

        // MC PlayerList.broadcastAll(new ClientboundPlayerInfoUpdatePacket(
        // UPDATE_GAME_MODE, player)).
        void BroadcastGameMode(const ServerPlayer& player);

        // MC ServerPlayer.setCamera: `entityId` is a mob or player id, or
        // ServerPlayer::kSelfCamera for the player's own eyes. The player is
        // moved to the camera (a real teleport the client acks) and told
        // which entity to look through (SetCameraS2C). No-op when unchanged.
        void SetCamera(PlayerSession& session, int32_t entityId);

        // MC ServerPlayer.tick's camera block: while looking through another
        // entity the player rides along with it (absSnapTo, so the chunks and
        // tracking follow), and is given their own eyes back when it dies or
        // leaves, or when they press sneak (wantsToStopRiding).
        void TickCamera(PlayerSession& session);

        // MC handleSpectatorAction: a spectator's attack click. With an
        // entity in reach (entity interaction range + 3) that is pickable, it
        // becomes the camera.
        void HandleSpectatorAction(PlayerSession& session, const Network::SpectatorActionC2SPacket& packet);

        // MC handleTeleportToEntityPacket: the spectator menu's "teleport to
        // player", into whichever level that player is in.
        void HandleTeleportToEntity(PlayerSession& session, const Network::TeleportToEntityC2SPacket& packet);

        // MC ServerPlayerGameMode.useItemOn's SPECTATOR branch, for the block
        // at `pos`: its menu provider opens (read-only — every click in it is
        // refused), a portal block takes the spectator through, anything else
        // is a PASS. True when the click was consumed.
        bool UseItemOn(PlayerSession& session, Game::World& world, const glm::ivec3& pos);

        // MC ChunkMap.skipPlayer: a spectator with spectatorsGenerateChunks
        // off neither holds a chunk ticket nor asks for a chunk to load or
        // generate — they see what other players keep loaded.
        bool SkipsChunkLoading(const ServerPlayer& player);

    } // namespace Spectator

} // namespace Server
