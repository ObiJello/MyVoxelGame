// File: src/client/renderer/gui/screens/SkyboxSelectScreen.hpp
//
// The skybox picker: a scrolling grid of preview cards, one per sky, in
// place of the old two-column list of names (which, with twenty title
// panoramas in it, read as "Panorama 1 … Panorama 20" and told you nothing
// until you equipped one).
//
//   • Each card shows a strip of the set's left, front and right faces —
//     the horizon band, where skies differ most — with the name under it.
//     Clicking a card applies it at once, so the world behind the menu
//     shows the choice while the picker stays open; Done keeps it.
//   • Order: Vanilla, The End, the player's own sets (<game dir>/skyboxes),
//     the shipped sets, and — only when "Show Panoramas" is on — the title
//     screen panoramas, under their own header. The one in use is always
//     listed.
//   • A footer button opens the player's skyboxes folder in the file
//     browser, which is the whole "download your own" workflow: make a
//     folder there and drop panorama_0..5 in (png, jpg, bmp or tga), or
//     drop in an unzipped Minecraft resource pack with an OptiFine custom
//     sky (assets/minecraft/optifine/sky/world0/sky<n>.properties). The
//     screen rescans the folders once a second while it is open, so the
//     new set appears without closing it.
//
// Previews are decoded on a worker thread (three 2048² PNGs per set is
// hundreds of milliseconds; twenty sets would freeze the menu for seconds)
// and uploaded as they arrive; the cache lives for the process so the
// second opening is instant.
#pragma once

#include "Screen.hpp"
#include "Widgets.hpp"
#include "../../backend/RenderTypes.hpp"
#include "../../environment/SkyRenderer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Render {

    // Preview textures for the picker, keyed by skybox id.
    class SkyboxThumbnails {
    public:
        // Strip geometry: three 64×64 faces side by side.
        static constexpr int kFace  = 64;
        static constexpr int kFaces = 3;

        static SkyboxThumbnails& Get();

        // The set's preview, or INVALID_TEXTURE while it is still decoding
        // (the first call queues it). Special skies ("vanilla") have none.
        TextureHandle Request(const SkyboxInfo& info);
        // Main thread, once per frame while the picker is open: turns
        // finished decodes into textures.
        void Poll();
        // Frees every texture and stops the worker. Called at renderer
        // shutdown, before the backend goes away.
        void Shutdown();

    private:
        SkyboxThumbnails() = default;
        ~SkyboxThumbnails() { Shutdown(); }

        // One preview face: an image file and the part of it to use
        // (fractions; the whole image for a six-face set, one 3×2 cell of
        // an OptiFine sky texture).
        struct Face    { std::string path; float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f; };
        struct Job     { std::string id; std::vector<Face> faces; };
        struct Decoded { std::string id; int width = 0, height = 0; std::vector<uint8_t> rgba; };

        void EnsureWorker();
        void WorkerMain();
        static bool DecodeStrip(const Job& job, Decoded& out);

        std::unordered_map<std::string, TextureHandle> m_textures;   // INVALID while pending
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_failedAt;
        std::mutex              m_mutex;
        std::condition_variable m_cv;
        std::deque<Job>         m_queue;
        std::vector<Decoded>    m_done;
        std::thread             m_worker;
        std::atomic<bool>       m_stop{false};
    };

    // The grid widget: cards, headers, scrolling. Owned by the screen.
    class SkyboxGrid : public AbstractWidget {
    public:
        SkyboxGrid(int x, int y, int width, int height);

        // Rebuild the cards from a fresh discovery.
        void Populate(bool showPanoramas);

        const std::vector<std::string>* TooltipAt(double mx, double my) override;
        void OnClick(double mouseX, double mouseY) override;
        bool OnScroll(double deltaY) override;

        bool HasPanoramas() const { return m_panoramaCount > 0; }

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        static constexpr int kCardW     = 100;
        static constexpr int kCardH     = 50;
        static constexpr int kGap       = 6;
        static constexpr int kColumns   = 3;
        static constexpr int kHeaderH   = 16;
        static constexpr int kPreviewW  = kCardW - 8;
        static constexpr int kPreviewH  = 30;
        static constexpr int kScrollbarW = 6;

        struct Cell {
            bool        header = false;
            std::string text;              // header text, or the card label
            SkyboxInfo  info;              // cards only
            std::vector<std::string> tooltip;
            // Laid out by Layout(): screen-space rectangle.
            int x = 0, y = 0, w = 0, h = 0;
        };

        void   Layout();
        int    ContentHeight() const;
        double MaxScroll() const;
        void   ClampScroll();
        int    GridLeft() const { return m_x + m_width / 2 - (kColumns * kCardW + (kColumns - 1) * kGap) / 2; }
        int    ScrollbarX() const { return GridLeft() + kColumns * kCardW + (kColumns - 1) * kGap + 8; }
        const Cell* CellAt(double mx, double my) const;
        void   DrawCard(GuiGraphics& g, const Cell& cell, bool hovered, bool selected);
        static std::string Ellipsize(GuiGraphics& g, const std::string& text, int maxWidth);

        std::vector<Cell> m_cells;
        double m_scroll = 0.0;
        int    m_panoramaCount = 0;
        int    m_contentHeight = 0;
    };

    class SkyboxSelectScreen : public Screen {
    public:
        SkyboxSelectScreen() : Screen("Select Skybox") {}
        void Init() override;
        void Tick() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        void OnClose() override;

    private:
        static constexpr int HEADER_H = 33;
        static constexpr int FOOTER_H = 33;

        SkyboxGrid* m_grid = nullptr;
        Button*     m_panoramaToggle = nullptr;
        // Ids seen at the last rescan (see Tick).
        std::vector<std::string> m_knownIds;
        int m_rescanTicks = 0;
        // Sticky for the session: once you asked for the panoramas you
        // keep seeing them until you hide them again.
        static bool s_showPanoramas;
    };

} // namespace Render
