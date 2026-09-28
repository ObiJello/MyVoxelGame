// File: src/client/entity/ClientFireworks.hpp
//
// The client half of an attached firework rocket (FireworkRocket.hpp's
// FireworkAttachHost): where the entity riding a rocket is, whether it
// glides, where it looks, and — for the local player, whose movement this
// client owns (MC LocalPlayer) — the boosted velocity the rocket hands back.
// The client's mob level holds no players, so the rocket asks here.
#pragma once

#include <cstdint>

namespace Game { class ClientPlayer; }

namespace Client::Fireworks {

    // Installs the host for the local player (PlatformMain, once the player
    // object exists). Safe to call again with the same player.
    void Install(Game::ClientPlayer* localPlayer);

    // The local player's 20 Hz glide bookkeeping (PlatformMain's client
    // tick): MC LocalPlayer.onSyncedDataUpdated starts ElytraOnPlayerSound-
    // Instance — item.elytra.flying, looped, silent for a second, then
    // swelling with the glide's speed — when a glide begins.
    void TickLocalPlayer(const Game::ClientPlayer& localPlayer);

    // ── Elytra-boost diagnostics (log lines, no behaviour) ────────────────
    // A rocket's synched data arrived (FireworkRocketDataS2C): `found` is
    // whether the client already had the entity. Logs once when a rocket
    // attaches to the local player, and warns when one names the local
    // player but has no client copy to attach.
    void OnRocketData(int32_t rocketId, int32_t attachedToId, int lifetime, bool found);
    // The local player used a rocket in the air while gliding (the boost
    // request); a warning follows if no rocket attaches within 5 ticks.
    void NoteBoostUse();

} // namespace Client::Fireworks
