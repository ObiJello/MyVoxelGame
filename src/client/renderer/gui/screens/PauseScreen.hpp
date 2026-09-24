// File: src/client/renderer/gui/screens/PauseScreen.hpp
//
// The in-game ESC menu — C++ counterpart of MC's PauseScreen ("Game Menu").
// Drawn over the live world with the transparent-gradient background (the
// world keeps rendering and, since the integrated server is multiplayer by
// design, keeps TICKING — same as vanilla with "Open to LAN" active).
//
//   Back to Game            → pops the screen, recaptures the mouse
//   Advancements/Statistics → placeholders (disabled, vanilla layout)
//   Options...              → the full options tree (same screens as title)
//   Open to LAN             → disabled; the server is already listening
//   Save and Quit to Title  → tears the world session down (server save runs
//                             in the session shutdown sequence) and returns
//                             to the title screen via PlatformMain's outer
//                             session loop.
#pragma once

#include "Screen.hpp"

namespace Render {

    class PauseScreen : public Screen {
    public:
        PauseScreen() : Screen("Game Menu") {}
        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

        // "Save and Quit to Title" clears the menu on the click, as MC does:
        // with the panorama's faces captured under the menu (LeaveCapture),
        // the world is the title's picture on the very next frame. (It used
        // to fade out over the click-time capture, which no longer exists.)
    };

} // namespace Render
