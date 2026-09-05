// File: src/client/renderer/gui/screens/SkyboxSelectScreen.cpp
#include "SkyboxSelectScreen.hpp"

#include "OptionsScreens.hpp"          // ApplyWorldSkySelection
#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "../../backend/RenderBackend.hpp"
#include "platform/GameDirectory.hpp"
#include "common/core/Log.hpp"
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    // ═══════════════════════════ SkyboxThumbnails ═══════════════════════════

    SkyboxThumbnails& SkyboxThumbnails::Get() {
        static SkyboxThumbnails s_instance;
        return s_instance;
    }

    TextureHandle SkyboxThumbnails::Request(const SkyboxInfo& info) {
        auto it = m_textures.find(info.id);
        if (it != m_textures.end()) return it->second;
        // A decode that failed (a file still being copied in, a broken
        // image) is retried, but not every frame.
        if (auto failed = m_failedAt.find(info.id); failed != m_failedAt.end()) {
            if (std::chrono::steady_clock::now() - failed->second < std::chrono::seconds(3)) {
                return INVALID_TEXTURE;
            }
            m_failedAt.erase(failed);
        }

        Job job;
        job.id = info.id;
        if (info.id == "end") {
            // One tile of the End's starfield, repeated across the strip
            // the way the sky tiles it.
            const std::string tile = PlatformMain::GetAssetPath("assets/textures/environment/end_sky.png");
            for (int i = 0; i < kFaces; ++i) job.faces.push_back(Face{tile});
        } else if (info.kind == SkyboxKind::OptiFine) {
            // The same horizon band out of the first layer's 3×2 texture:
            // west, north and east are cells 3, 4 and 5 (the bottom row,
            // numbered row-major from the top left; a cell is a third wide
            // and half tall). The sides sit in the texture the way they
            // face, so no turning.
            if (info.optifineTexture.empty()) return INVALID_TEXTURE;
            for (int cell : {3, 4, 5}) {
                Face face;
                face.path = info.optifineTexture;
                face.u0 = static_cast<float>(cell % 3) / 3.0f;  face.u1 = face.u0 + 1.0f / 3.0f;
                face.v0 = static_cast<float>(cell / 3) / 2.0f;  face.v1 = face.v0 + 0.5f;
                job.faces.push_back(face);
            }
        } else if (!info.dir.empty()) {
            // Left, front, right: the horizon band as you would see it
            // turning your head, in the panorama_N convention
            // (3 = left/-X, 0 = front/-Z, 1 = right/+X).
            for (int face : {3, 0, 1}) {
                const std::string path = SkyboxFacePath(info.dir, face);
                if (path.empty()) return INVALID_TEXTURE;   // incomplete set: no preview
                job.faces.push_back(Face{path});
            }
        } else {
            return INVALID_TEXTURE;   // "vanilla": drawn procedurally
        }
        m_textures[info.id] = INVALID_TEXTURE;   // pending
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back(std::move(job));
        }
        EnsureWorker();
        m_cv.notify_one();
        return INVALID_TEXTURE;
    }

    void SkyboxThumbnails::EnsureWorker() {
        if (m_worker.joinable()) return;
        m_stop = false;
        m_worker = std::thread([this] { WorkerMain(); });
    }

    void SkyboxThumbnails::WorkerMain() {
        // stb's flip flag is process-global by default; the thread-local
        // form keeps another loader's setting from turning previews over.
        stbi_set_flip_vertically_on_load_thread(0);
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stop.load() || !m_queue.empty(); });
                if (m_stop.load()) return;
                job = std::move(m_queue.front());
                m_queue.pop_front();
            }
            Decoded out;
            out.id = job.id;
            if (!DecodeStrip(job, out)) {
                out.rgba.clear();   // an empty decode = "failed", handed back so the card stops waiting
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_done.push_back(std::move(out));
        }
    }

    bool SkyboxThumbnails::DecodeStrip(const Job& job, Decoded& out) {
        const int faces = static_cast<int>(job.faces.size());
        if (faces <= 0) return false;
        out.width  = kFace * faces;
        out.height = kFace;
        out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);

        // The faces of an OptiFine sky all come out of one file: decode it
        // once (a 3072×2048 PNG is most of the job's time).
        std::string    loadedPath;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        auto release = [&] { if (px) { stbi_image_free(px); px = nullptr; } loadedPath.clear(); };

        for (int f = 0; f < faces; ++f) {
            const Face& face = job.faces[f];
            if (face.path != loadedPath) {
                release();
                int ch = 0;
                px = stbi_load(face.path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
                if (!px || w <= 0 || h <= 0) { release(); return false; }
                loadedPath = face.path;
            }
            // The source rectangle this face uses, in texels.
            const int rx0 = std::clamp(static_cast<int>(std::lround(face.u0 * w)), 0, w - 1);
            const int ry0 = std::clamp(static_cast<int>(std::lround(face.v0 * h)), 0, h - 1);
            const int rx1 = std::clamp(static_cast<int>(std::lround(face.u1 * w)), rx0 + 1, w);
            const int ry1 = std::clamp(static_cast<int>(std::lround(face.v1 * h)), ry0 + 1, h);
            const int rw = rx1 - rx0, rh = ry1 - ry0;
            // Area-average the face down to kFace×kFace. A 2048² PNG is 32×32
            // source texels per output texel; summing them is what keeps
            // star fields from aliasing into noise.
            for (int oy = 0; oy < kFace; ++oy) {
                const int sy0 = ry0 + oy * rh / kFace, sy1 = std::max(sy0 + 1, ry0 + (oy + 1) * rh / kFace);
                for (int ox = 0; ox < kFace; ++ox) {
                    const int sx0 = rx0 + ox * rw / kFace, sx1 = std::max(sx0 + 1, rx0 + (ox + 1) * rw / kFace);
                    uint64_t r = 0, g = 0, b = 0, n = 0;
                    for (int sy = sy0; sy < sy1; ++sy) {
                        const unsigned char* row = px + (static_cast<size_t>(sy) * w + sx0) * 4;
                        for (int sx = sx0; sx < sx1; ++sx, row += 4) {
                            r += row[0]; g += row[1]; b += row[2]; ++n;
                        }
                    }
                    uint8_t* dst = out.rgba.data() +
                        (static_cast<size_t>(oy) * out.width + (static_cast<size_t>(f) * kFace + ox)) * 4;
                    dst[0] = static_cast<uint8_t>(r / n);
                    dst[1] = static_cast<uint8_t>(g / n);
                    dst[2] = static_cast<uint8_t>(b / n);
                    dst[3] = 255;
                }
            }
        }
        release();
        return true;
    }

    void SkyboxThumbnails::Poll() {
        std::vector<Decoded> done;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            done.swap(m_done);
        }
        if (done.empty() || !g_renderBackend) return;
        for (Decoded& d : done) {
            auto it = m_textures.find(d.id);
            if (it == m_textures.end()) continue;   // dropped by a Shutdown meanwhile
            if (d.rgba.empty()) {
                // Failed: forget it so the card shows its fallback and a
                // later Request (after the player fixes the files) retries.
                m_textures.erase(it);
                m_failedAt[d.id] = std::chrono::steady_clock::now();
                continue;
            }
            TextureHandle t = g_renderBackend->CreateTexture2D(d.width, d.height, TextureFormat::RGBA8,
                                                               d.rgba.data());
            if (t != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(t, TextureFilter::Linear, TextureFilter::Linear);
                g_renderBackend->SetTextureWrap(t, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            it->second = t;
        }
    }

    void SkyboxThumbnails::Shutdown() {
        if (m_worker.joinable()) {
            m_stop = true;
            m_cv.notify_all();
            m_worker.join();
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.clear();
            m_done.clear();
        }
        if (g_renderBackend) {
            for (auto& [id, t] : m_textures) {
                if (t != INVALID_TEXTURE) g_renderBackend->DestroyTexture(t);
            }
        }
        m_textures.clear();
        m_failedAt.clear();
    }

    // ═══════════════════════════ SkyboxGrid ═════════════════════════════════

    SkyboxGrid::SkyboxGrid(int x, int y, int width, int height)
        : AbstractWidget(x, y, width, height, "") {}

    void SkyboxGrid::Populate(bool showPanoramas) {
        m_cells.clear();
        m_panoramaCount = 0;
        const std::string current = g_skyRenderer.CurrentSkybox();
        const std::vector<SkyboxInfo> all = DiscoverSkyboxes();

        auto header = [&](const std::string& text) {
            Cell c; c.header = true; c.text = text;
            m_cells.push_back(std::move(c));
        };
        auto card = [&](const SkyboxInfo& info) {
            Cell c;
            c.info = info;
            c.text = info.label;
            switch (info.source) {
                case SkyboxSource::Special:
                    c.tooltip = info.id == "end"
                        ? std::vector<std::string>{"The End's starfield, as the",
                                                   "End dimension itself shows it."}
                        : std::vector<std::string>{"Minecraft's own sky: day and",
                                                   "night, sun, moon and stars."};
                    break;
                case SkyboxSource::User:
                    c.tooltip = {"From your skyboxes folder:", "skyboxes/" + info.id};
                    break;
                case SkyboxSource::Builtin:
                    c.tooltip = {"Shipped with the game."};
                    break;
                case SkyboxSource::Panorama:
                    c.tooltip = {"A title screen panorama,", "usable as a sky."};
                    break;
            }
            if (info.kind == SkyboxKind::OptiFine) {
                // A resource pack: its sky is layered over the vanilla
                // sky (day, sun, moon, stars stay) and fades on the pack's
                // own schedule, so Sky Behavior does not apply to it.
                c.tooltip.push_back("Minecraft resource pack (OptiFine sky, " +
                                    std::to_string(info.optifineLayers) +
                                    (info.optifineLayers == 1 ? " layer)." : " layers)."));
                c.tooltip.push_back("Drawn over the vanilla sky; fades and");
                c.tooltip.push_back("turns on the pack's own schedule.");
            }
            m_cells.push_back(std::move(c));
        };

        // Specials and the shipped sets are one group; the player's own
        // sets get a header so they can tell what came from their folder.
        std::vector<const SkyboxInfo*> special, user, builtin, panorama;
        for (const SkyboxInfo& info : all) {
            switch (info.source) {
                case SkyboxSource::Special:  special.push_back(&info);  break;
                case SkyboxSource::User:     user.push_back(&info);     break;
                case SkyboxSource::Builtin:  builtin.push_back(&info);  break;
                case SkyboxSource::Panorama: panorama.push_back(&info); break;
            }
        }
        m_panoramaCount = static_cast<int>(panorama.size());

        for (const SkyboxInfo* s : special) card(*s);
        for (const SkyboxInfo* s : builtin) card(*s);

        header(user.empty() ? "Your skyboxes: none yet (drop folders into the skyboxes folder)"
                            : "Your skyboxes");
        for (const SkyboxInfo* s : user) card(*s);

        if (!panorama.empty()) {
            std::vector<const SkyboxInfo*> shown;
            for (const SkyboxInfo* s : panorama) {
                if (showPanoramas || s->id == current) shown.push_back(s);
            }
            if (!shown.empty()) {
                header(showPanoramas ? "Title screen panoramas" : "Title screen panoramas (in use)");
                for (const SkyboxInfo* s : shown) card(*s);
            }
        }
        m_scroll = 0.0;
        Layout();
    }

    void SkyboxGrid::Layout() {
        const int left = GridLeft();
        int y = 4;
        int column = 0;
        int rowTop = y;
        auto endRow = [&]() {
            if (column > 0) { y = rowTop + kCardH + kGap; column = 0; }
        };
        for (Cell& c : m_cells) {
            if (c.header) {
                endRow();
                c.x = m_x; c.w = m_width; c.y = y; c.h = kHeaderH;
                y += kHeaderH;
                rowTop = y;
                continue;
            }
            if (column == 0) rowTop = y;
            c.x = left + column * (kCardW + kGap);
            c.y = rowTop;
            c.w = kCardW;
            c.h = kCardH;
            if (++column == kColumns) { y = rowTop + kCardH + kGap; column = 0; }
        }
        endRow();
        m_contentHeight = y + 4;
        ClampScroll();
    }

    int SkyboxGrid::ContentHeight() const { return m_contentHeight; }

    double SkyboxGrid::MaxScroll() const {
        const double max = static_cast<double>(m_contentHeight) - m_height;
        return max > 0.0 ? max : 0.0;
    }

    void SkyboxGrid::ClampScroll() { m_scroll = std::clamp(m_scroll, 0.0, MaxScroll()); }

    const SkyboxGrid::Cell* SkyboxGrid::CellAt(double mx, double my) const {
        if (!ContainsPoint(mx, my)) return nullptr;
        const double ly = my - m_y + m_scroll;
        for (const Cell& c : m_cells) {
            if (c.header) continue;
            if (mx >= c.x && mx < c.x + c.w && ly >= c.y && ly < c.y + c.h) return &c;
        }
        return nullptr;
    }

    const std::vector<std::string>* SkyboxGrid::TooltipAt(double mx, double my) {
        const Cell* c = CellAt(mx, my);
        return (c && !c->tooltip.empty()) ? &c->tooltip : nullptr;
    }

    void SkyboxGrid::OnClick(double mouseX, double mouseY) {
        const Cell* c = CellAt(mouseX, mouseY);
        if (!c) return;
        // Applied at once, as a preview: the world behind the menu shows
        // it, and the choice is written to the world's record then and
        // there, so Done and Esc both keep it.
        ApplyWorldSkySelection(c->info.id, g_skyRenderer.CurrentSkyboxMode());
    }

    bool SkyboxGrid::OnScroll(double deltaY) {
        if (MaxScroll() <= 0.0) return false;
        m_scroll -= deltaY * (kCardH + kGap) * 0.5;
        ClampScroll();
        return true;
    }

    std::string SkyboxGrid::Ellipsize(GuiGraphics& g, const std::string& text, int maxWidth) {
        if (g.GetStringWidth(text) <= maxWidth) return text;
        std::string t = text;
        while (!t.empty() && g.GetStringWidth(t + "...") > maxWidth) t.pop_back();
        return t + "...";
    }

    void SkyboxGrid::DrawCard(GuiGraphics& g, const Cell& cell, bool hovered, bool selected) {
        const int x = cell.x, y = cell.y - static_cast<int>(m_scroll);
        // Card body.
        g.Fill(x, y, x + cell.w, y + cell.h, hovered ? 0xC0202020 : 0xA0101010);
        // Preview strip: the middle band of the three faces (the horizon).
        const int px0 = x + 4, py0 = y + 4;
        const int px1 = px0 + kPreviewW, py1 = py0 + kPreviewH;
        const TextureHandle tex = SkyboxThumbnails::Get().Request(cell.info);
        if (cell.info.id == "vanilla") {
            // Noon sky: MC's sky colour above, the horizon haze below, and a
            // sun. Drawn, not loaded — there is no image of the vanilla sky.
            g.FillGradient(px0, py0, px1, py1, 0xFF78A7FF, 0xFFC6DBFF);
            g.Fill(px0 + kPreviewW / 2 - 4, py0 + 4, px0 + kPreviewW / 2 + 4, py0 + 12, 0xFFFFF4C8);
        } else if (tex != INVALID_TEXTURE) {
            // The strip is 3:1; the preview is ~3:1 too, so show the band
            // v ∈ [0.25, 0.75] of it: half a face's height around the horizon.
            const float vHalf = 0.5f * (static_cast<float>(kPreviewH) / kPreviewW) *
                                (static_cast<float>(SkyboxThumbnails::kFaces));
            const float v0 = std::max(0.0f, 0.5f - vHalf), v1 = std::min(1.0f, 0.5f + vHalf);
            g.Blit(tex, px0, py0, px1, py1, 0.0f, v0, 1.0f, v1);
        } else {
            // Still decoding (or unreadable): a quiet placeholder.
            g.Fill(px0, py0, px1, py1, 0xFF262626);
            g.DrawCenteredString("...", px0 + kPreviewW / 2,
                                 py0 + (kPreviewH - FontRenderer::LINE_HEIGHT) / 2, 0xFF808080);
        }
        // Label.
        const std::string label = Ellipsize(g, cell.text, kPreviewW);
        g.DrawCenteredString(label, x + cell.w / 2, py1 + 4,
                             selected ? 0xFFFFFFA0 : (hovered ? 0xFFFFFFFF : 0xFFE0E0E0));
        // Frame: the selection reads at a glance, hover a step behind it.
        if (selected) {
            g.RenderOutline(x, y, cell.w, cell.h, 0xFFFFFFFF);
            g.RenderOutline(x + 1, y + 1, cell.w - 2, cell.h - 2, 0xFFFFFFFF);
        } else {
            g.RenderOutline(x, y, cell.w, cell.h, hovered ? 0xFFB0B0B0 : 0xFF3A3A3A);
        }
    }

    void SkyboxGrid::RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float) {
        ClampScroll();
        SkyboxThumbnails::Get().Poll();
        g.Fill(m_x, m_y, m_x + m_width, m_y + m_height, 0x77000000);

        const std::string current = g_skyRenderer.CurrentSkybox();
        const Cell* hovered = CellAt(mouseX, mouseY);
        g.EnableScissor(m_x, m_y, m_x + m_width, m_y + m_height);
        for (const Cell& c : m_cells) {
            const int top = m_y + c.y - static_cast<int>(m_scroll);
            if (top + c.h < m_y || top > m_y + m_height) continue;
            if (c.header) {
                g.DrawCenteredString(c.text, m_x + m_width / 2,
                                     top + (kHeaderH - FontRenderer::LINE_HEIGHT) / 2 + 2, 0xFFA0A0A0);
                continue;
            }
            // Cells are laid out in list space; DrawCard subtracts the
            // scroll itself, so hand it the widget-space rectangle.
            Cell placed = c;
            placed.y = m_y + c.y;
            DrawCard(g, placed, &c == hovered, c.info.id == current);
        }
        g.DisableScissor();

        if (MaxScroll() > 0.0) {
            const int sx = ScrollbarX();
            g.BlitSprite("widget/scroller_background", sx, m_y, kScrollbarW, m_height);
            const double thumbH = std::max(32.0, static_cast<double>(m_height) * m_height / m_contentHeight);
            const double frac = m_scroll / MaxScroll();
            const int thumbY = m_y + static_cast<int>(frac * (m_height - thumbH));
            g.BlitSprite("widget/scroller", sx, thumbY, kScrollbarW, static_cast<int>(thumbH));
        }
    }

    // ═══════════════════════════ SkyboxSelectScreen ═════════════════════════

    bool SkyboxSelectScreen::s_showPanoramas = false;

    void SkyboxSelectScreen::Init() {
        m_grid = AddWidget(new SkyboxGrid(0, HEADER_H, m_width, m_height - HEADER_H - FOOTER_H));
        m_grid->Populate(s_showPanoramas);
        m_knownIds.clear();
        for (const SkyboxInfo& info : DiscoverSkyboxes()) m_knownIds.push_back(info.id);
        m_rescanTicks = 0;

        // Footer: open the folder, toggle the panoramas, Done. Three
        // 98-wide buttons with MC's 4px gaps, centred as a group.
        const int w = 98, gap = 4;
        const int footerY = m_height - FOOTER_H / 2 - 10;
        const int left = m_width / 2 - (w * 3 + gap * 2) / 2;
        auto* folder = AddWidget(new Button(left, footerY, w, 20, "Skyboxes Folder...", [] {
            const std::string dir = UserSkyboxDirectory();
            if (!Platform::GameDirectory::OpenInFileBrowser(dir)) {
                Log::Warning("[Skybox] Could not open %s in the file browser", dir.c_str());
            }
        }));
        folder->SetTooltip({"Opens your skyboxes folder. Put each",
                            "sky in its own folder there as six",
                            "faces named panorama_0.png to",
                            "panorama_5.png, then reopen this screen."});
        m_panoramaToggle = AddWidget(new Button(left + w + gap, footerY, w, 20,
            s_showPanoramas ? "Hide Panoramas" : "Show Panoramas", [this] {
                s_showPanoramas = !s_showPanoramas;
                m_panoramaToggle->SetMessage(s_showPanoramas ? "Hide Panoramas" : "Show Panoramas");
                m_grid->Populate(s_showPanoramas);
            }));
        m_panoramaToggle->SetTooltip({"The title screen's panoramas can be",
                                      "used as skies too. Hidden by default;",
                                      "the one in use is always listed."});
        m_panoramaToggle->active = m_grid->HasPanoramas();
        AddWidget(new Button(left + (w + gap) * 2, footerY, w, 20, "Done", [this] { OnClose(); }));
    }

    void SkyboxSelectScreen::Tick() {
        // Once a second, look again: a folder dropped into the skyboxes
        // folder while this screen is up appears without reopening it,
        // and one removed disappears. Only the set of ids is compared, so
        // a steady folder costs three directory listings a second.
        if (++m_rescanTicks < 20) return;
        m_rescanTicks = 0;
        std::vector<std::string> ids;
        for (const SkyboxInfo& info : DiscoverSkyboxes()) ids.push_back(info.id);
        if (ids != m_knownIds) {
            m_knownIds = std::move(ids);
            if (m_grid) m_grid->Populate(s_showPanoramas);
            if (m_panoramaToggle && m_grid) m_panoramaToggle->active = m_grid->HasPanoramas();
        }
    }

    void SkyboxSelectScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2, (HEADER_H - FontRenderer::LINE_HEIGHT) / 2, 0xFFFFFFFF);
        RenderMenuSeparators(g, m_width, HEADER_H - 2, m_height - FOOTER_H);
    }

    void SkyboxSelectScreen::OnClose() {
        Platform::g_gameSettings.Save();
        Screen::OnClose();
    }

} // namespace Render
