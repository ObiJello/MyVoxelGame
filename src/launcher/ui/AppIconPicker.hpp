// File: src/launcher/ui/AppIconPicker.hpp
//
// Settings → App icon: the block the game's Dock / window icon shows. TNT is
// the default (the icon the game ships pre-rendered, no flag passed); Random
// draws a new full block every launch; any other full block is drawn at
// launch the same way. The choice is launcher.json "app_icon" and reaches the
// game as `--icon <id|random>` (PlatformMain SetAppIcon).
//
// The row opens a searchable grid of every full block, each tile a thumbnail
// drawn by Render::BlockIcon — the game's own renderer, from the installed
// game's assets — so a tile is exactly the icon that block gives the game.
// The block list loads on a worker thread the first time the row is drawn;
// thumbnails are drawn on the UI thread within a few milliseconds a frame,
// only for tiles on screen.
#pragma once

#include "client/renderer/gui/BlockIcon.hpp"

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

typedef unsigned int GLuint;

namespace Launcher {

    class AppIconPicker {
    public:
        AppIconPicker() = default;
        ~AppIconPicker();
        AppIconPicker(const AppIconPicker&) = delete;
        AppIconPicker& operator=(const AppIconPicker&) = delete;

        // The installed game's assets directory; read once it exists.
        void SetAssetsDir(std::filesystem::path assetsDir);

        // The Settings row at the cursor, `width` wide. `choice` is the
        // stored value: a block id, Render::BlockIcon::kRandom, or the
        // default kDefaultBlock; a pick writes it.
        void DrawRow(std::string& choice, float width);

        // Frees the thumbnails' GL textures. Call while the context is
        // current.
        void ReleaseGpu();

    private:
        struct Thumb {
            GLuint texture = 0;
        };

        void StartLoading();
        // The thumbnail for `id`, drawn now if this frame's budget allows;
        // 0 while it waits.
        GLuint ThumbFor(const std::string& id, int pixels);
        const Render::BlockIcon::Model* Find(const std::string& id) const;
        std::string LabelFor(const std::string& choice) const;
        void DrawTile(ImDrawList* dl, const char* key, const std::string& id, const std::string& label,
                      ImVec2 pos, float size, bool selected, bool& clicked);
        void DrawGrid(std::string& choice);

        std::filesystem::path m_assetsDir;

        // Loading (worker → UI).
        std::thread m_loader;
        std::atomic<bool> m_loaded{false};   // the worker is done (set by the worker)
        bool m_loadStarted = false;
        double m_lastAssetsCheck = -10.0;    // ImGui time of the last look for the assets
        bool m_ready = false;                // joined; m_blocks / m_index readable (UI thread)
        std::vector<Render::BlockIcon::Model> m_blocks;   // the worker's, read once m_loaded
        std::unordered_map<std::string, size_t> m_index; // id → m_blocks, built on first sight

        std::unordered_map<std::string, Thumb> m_thumbs;   // "<id>@<pixels>"
        int m_thumbsDrawn = 0;   // this frame's
        std::chrono::steady_clock::time_point m_thumbDeadline;

        char m_search[48] = "";
    };

} // namespace Launcher
