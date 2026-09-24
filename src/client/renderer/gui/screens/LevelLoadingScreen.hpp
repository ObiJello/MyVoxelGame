// File: src/client/renderer/gui/screens/LevelLoadingScreen.hpp
//
// MC LevelLoadingScreen (Reason.OTHER): "Downloading terrain…" while the
// client's LevelLoadTracker waits for the player's own section to compile;
// the host loop closes it the tick the tracker says the level is ready
// (LevelLoadingScreen.tick → onClose). Vanilla paints the title panorama
// blurred behind the text; this engine has no in-world blur pass, so the
// opaque menu tile stands in. The chunk-status map and progress bar are
// the server's spawn-preparation report, which has no equivalent here.
#pragma once

#include "Screen.hpp"

namespace Render {

    class LevelLoadingScreen : public Screen {
    public:
        LevelLoadingScreen() : Screen("") {}
        void Init() override {}
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        bool ShouldCloseOnEsc() const override { return false; }
        bool IsPauseScreen()   const override { return false; }
    };

} // namespace Render
