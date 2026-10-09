// File: src/launcher/GameRenderer.hpp
#pragma once

#include <string>

// The game's render backends as the launcher offers them: what the picker
// shows, what launcher.json stores ("renderer"), and the flag the game is
// started with (PlatformMain.cpp parses --vulkan / --metal; neither is
// OpenGL).
namespace Launcher {

    enum class GameRenderer : int { OpenGL = 0, Vulkan = 1, Metal = 2 };

    struct GameRendererInfo {
        GameRenderer id;
        const char*  label;   // the picker's name
        const char*  note;    // one line under it in the menu
        const char*  slug;    // launcher.json "renderer"
        const char*  arg;     // game command-line flag ("" = none)
    };

    // The menu's options, in its order. Metal exists only on macOS.
    inline constexpr GameRendererInfo kGameRenderers[] = {
#ifdef __APPLE__
        {GameRenderer::Metal,  "Metal",  "Native macOS renderer",         "metal",  "--metal"},
#endif
        {GameRenderer::Vulkan, "Vulkan", "Vulkan (MoltenVK on macOS)",     "vulkan", "--vulkan"},
        {GameRenderer::OpenGL, "OpenGL", "Compatibility renderer",         "opengl", ""},
    };
    inline constexpr int kGameRendererCount = static_cast<int>(sizeof(kGameRenderers) / sizeof(kGameRenderers[0]));

    inline const GameRendererInfo& GameRendererInfoFor(GameRenderer r) {
        for (const GameRendererInfo& info : kGameRenderers) {
            if (info.id == r) return info;
        }
        return kGameRenderers[kGameRendererCount - 1];   // OpenGL, last on every platform
    }

    inline int GameRendererIndex(GameRenderer r) {
        for (int i = 0; i < kGameRendererCount; ++i) {
            if (kGameRenderers[i].id == r) return i;
        }
        return kGameRendererCount - 1;
    }

    // A stored slug; one this platform does not offer (Metal off macOS, an
    // unknown name) is `fallback`.
    inline GameRenderer GameRendererFromSlug(const std::string& slug, GameRenderer fallback) {
        for (const GameRendererInfo& info : kGameRenderers) {
            if (slug == info.slug) return info.id;
        }
        return fallback;
    }

} // namespace Launcher
