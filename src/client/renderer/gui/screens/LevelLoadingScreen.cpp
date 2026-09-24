// File: src/client/renderer/gui/screens/LevelLoadingScreen.cpp
#include "LevelLoadingScreen.hpp"
#include "../GuiGraphics.hpp"

namespace Render {

    void LevelLoadingScreen::Render(GuiGraphics& g, int, int, float) {
        RenderMenuBackgroundTexture(g, 0, 0, m_width, m_height);
        // MC LevelLoadingScreen.extractRenderState with no status view: the
        // label 50 GUI px above the centre.
        g.DrawCenteredString("Downloading terrain...", m_width / 2, m_height / 2 - 50, 0xFFFFFFFF);
    }

} // namespace Render
