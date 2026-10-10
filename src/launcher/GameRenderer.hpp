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

    // ── The user's own launch arguments (Settings → Launch arguments) ──
    // Appended to the game's command line as typed. A renderer flag among
    // them stands in for the picker's choice: --metal, --metal3, --metal4,
    // --vulkan, or --opengl (the game's "no flag", dropped from what is
    // passed). Anything else simply rides along with the picker's flag.
    inline bool LaunchArgTokenIsRenderer(const std::string& token) {
        return token == "--metal" || token == "--metal3" || token == "--metal4" ||
               token == "--vulkan" || token == "--opengl";
    }
    inline bool LaunchArgsNameARenderer(const std::string& args) {
        std::string token;
        for (size_t i = 0; i <= args.size(); ++i) {
            const bool sep = i == args.size() || args[i] == ' ' || args[i] == '\t' || args[i] == '\n';
            if (!sep) { token += args[i]; continue; }
            if (LaunchArgTokenIsRenderer(token)) return true;
            token.clear();
        }
        return false;
    }
    // The arguments as the game gets them: single-spaced, --opengl removed.
    inline std::string LaunchArgsForGame(const std::string& args) {
        std::string out, token;
        for (size_t i = 0; i <= args.size(); ++i) {
            const bool sep = i == args.size() || args[i] == ' ' || args[i] == '\t' || args[i] == '\n';
            if (!sep) { token += args[i]; continue; }
            if (!token.empty() && token != "--opengl") out += (out.empty() ? "" : " ") + token;
            token.clear();
        }
        return out;
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
