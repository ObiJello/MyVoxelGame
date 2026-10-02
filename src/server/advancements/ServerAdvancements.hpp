// File: src/server/advancements/ServerAdvancements.hpp
//
// MC ServerAdvancementManager + the advancement half of PlayerList: the
// loaded advancements (data/<ns>/advancement, AdvancementLoader.hpp, with the
// display icons decoded through the server's item-component codecs) and every
// online player's PlayerAdvancements, from join to disconnect.
//
// Lifecycle hooks (IntegratedServer / PlayerSession / the play listener):
//   OnPlayerJoined    load <world>/advancements/<uuid>.json
//   TickPlayer        MC ServerPlayer.doTick's advancement work: TICK,
//                     LOCATION every 20 ticks, LEVITATION, the fall and
//                     lava-ride tracking, INVENTORY_CHANGED from the
//                     inventory's diff, then flushDirty
//   SavePlayer        with the player's data (autosave, disconnect, stop)
//   OnPlayerLeft      save and forget
//
// Cheats: in this engine advancements do not progress in a world with cheats
// on (level.dat allowCommands) unless the world's `advancements_with_cheats`
// rule is on. While switched off, no trigger awards anything; the screen,
// the existing progress and /advancement keep working. Switched back on
// mid-session, the next tick evaluates each player as at login (listeners,
// LOCATION, INVENTORY_CHANGED for every held stack — TickPlayer).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace Network { struct SeenAdvancementsC2SPacket; }

namespace Game::Advancements { struct Registry; }

namespace Server {
    class PlayerSession;
    class ServerPlayer;
}

namespace Server::Advancements {

    class PlayerAdvancements;

    // The loaded advancements (loaded on first use, kept for the process).
    std::shared_ptr<const Game::Advancements::Registry> GetRegistry();

    // Whether triggers award progress in the running world.
    bool TriggersEnabled();

    // The player's progress, or null when the player has none loaded (not
    // joined yet, or already gone).
    PlayerAdvancements* Get(const ServerPlayer& player);
    PlayerAdvancements* Get(uint32_t playerId);

    void OnPlayerJoined(PlayerSession& session);
    void OnPlayerLeft(uint32_t playerId);
    void SavePlayer(const ServerPlayer& player);
    void SaveAll();
    // Every player's progress dropped (the world closed).
    void Reset();

    // Once per server tick per session, after the player ticked. `paused`:
    // the integrated server's pause (players do not tick, so neither do the
    // tick-driven triggers), but the flush still runs so command grants show.
    void TickPlayer(PlayerSession& session, int64_t serverTick, bool paused);

    // MC ServerGamePacketListenerImpl.handleSeenAdvancements.
    void HandleSeenAdvancements(PlayerSession& session, const Network::SeenAdvancementsC2SPacket& packet);

} // namespace Server::Advancements
