// File: src/client/renderer/gui/BossBarState.hpp
//
// The client's one boss bar — MC's Gui.events map of LerpingBossEvent,
// reduced to the single bar this engine's one boss (the ender dragon) ever
// shows. Written by ClientPacketHandler::onBossEventS2C (main thread), read
// by HudRenderer::RenderBossBar (also main thread) — same single-threaded
// contract as the rest of the HUD state.
#pragma once

#include <cstdint>
#include <string>

namespace Client {

    struct BossBarState {
        bool        visible = false;
        float       progress = 1.0f;
        // BossEventS2CPacket::Color ordinal — doubles as the sprite prefix
        // under assets/textures/gui/sprites/boss_bar/ (pink, blue, ...).
        uint8_t     color = 0;
        // 0 = smooth bar, else the notch count (6/10/12/20).
        uint8_t     notches = 0;
        std::string name;
    };

    // Defined in ClientPacketHandler.cpp.
    extern BossBarState g_bossBarState;

} // namespace Client
