// File: src/client/entity/LocalItemCooldowns.hpp
//
// The local player's ItemCooldowns (MC LocalPlayer.getCooldowns()) — the
// client's mirror of the server's table, fed by CooldownS2C
// (ClientPacketListener.handleItemCooldown) and ticked with the player.
//
// It draws the white sweep over a resting item (GuiGraphics.itemCooldown) and
// stops the client predicting a use the server is going to refuse
// (MultiPlayerGameMode.useItem). The server stays the authority: it refuses
// the use on its own table whatever this one says.
//
// The packet lands on the network I/O thread and the table is read by the
// main thread's HUD, so every entry point takes the module's lock.
#pragma once

#include <string>

namespace Game { struct ItemStack; }
namespace Network { struct CooldownS2CPacket; }

namespace Client::LocalItemCooldowns {

    // MC handleItemCooldown: duration 0 removes the group, else starts it.
    // Any thread.
    void OnPacket(const Network::CooldownS2CPacket& packet);

    // MC LocalPlayer.tick → getCooldowns().tick(). Main thread, 20 Hz.
    void Tick();

    // getCooldownPercent(item, partialTick). The partial tick is read off the
    // wall clock since the last Tick, so the sweep glides between ticks the
    // way MC's DeltaTracker makes it.
    float GetCooldownPercent(const Game::ItemStack& item);
    bool  IsOnCooldown(const Game::ItemStack& item);

    // A fresh table — leaving the world (MC builds a new LocalPlayer).
    void Clear();

} // namespace Client::LocalItemCooldowns
