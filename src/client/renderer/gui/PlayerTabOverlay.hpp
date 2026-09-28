// File: src/client/renderer/gui/PlayerTabOverlay.hpp
//
// MC PlayerTabOverlay — the list of online players shown while the player
// list key (Tab) is held. Every listed player (Client::PlayerInfoMap), up to
// 80, in MC's PLAYER_COMPARATOR order: spectators last, then by name
// (case-insensitive; there are no tab-list orders or teams here). Columns of
// at most 20 rows, a face per player, the name — italic and at 0x90 alpha
// for a spectator — and the ping bars from the server's latency.
//
// Not shown in a single-player world with nobody else in it (Hud: `!isLocal
// Server() || players.size() > 1 || objective != null`). No scoreboard
// objectives, header or footer exist in this port, so those parts of the
// layout are always empty.
#pragma once

#include "GuiGraphics.hpp"

namespace Render {

    class PlayerTabOverlay {
    public:
        // MC setVisible — the key's held state and whether the list may show.
        void SetVisible(bool visible) { m_visible = visible; }
        bool IsVisible() const { return m_visible; }

        // MC extractRenderState(graphics, screenWidth, scoreboard, null).
        void Render(GuiGraphics& g);

    private:
        bool m_visible = false;
    };

    PlayerTabOverlay& GetPlayerTabOverlay();

} // namespace Render
