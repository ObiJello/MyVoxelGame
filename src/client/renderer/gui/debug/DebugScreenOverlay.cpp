// File: src/client/renderer/gui/debug/DebugScreenOverlay.cpp
#include "DebugScreenOverlay.hpp"
#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "client/input/KeyMapping.hpp"
#include "client/network/NetworkClient.hpp"
#include "client/renderer/backend/RenderBackend.hpp"
#include "client/renderer/core/RenderOrigin.hpp"
#include "client/renderer/environment/EnvironmentState.hpp"
#include "client/world/ClientLevel.hpp"
#include "common/world/level/DimensionId.hpp"
#include "platform/GameDirectory.hpp"
#include "server/IntegratedServer.hpp"
#include "server/world/status/ChunkStatusManager.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>

namespace Render::DebugScreen {

    namespace {
        constexpr int MARGIN_RIGHT = 2;
        constexpr int MARGIN_LEFT  = 2;
        constexpr int MARGIN_TOP   = 2;
        constexpr uint32_t kTextColor  = 0xFFE0E0E0;   // -2039584
        constexpr uint32_t kShadeColor = 0x90505050;   // -1873784752

        // MC Window.calculateScale(guiScale, forceUnicode): the largest scale
        // that leaves a 320x240 GUI, capped by the setting (0 = uncapped).
        int CalculateScale(int setting, int fbW, int fbH) {
            int scale = 1;
            while (scale != setting && scale < fbW && scale < fbH &&
                   fbW / (scale + 1) >= 320 && fbH / (scale + 1) >= 240) {
                ++scale;
            }
            return scale;
        }

        // The debug renderers' text billboards, kept across the world pass
        // and drawn by the HUD pass.
        std::vector<BillboardText> g_billboards;

        std::string KeyName(const Input::KeyMapping* m) {
            return m ? m->key.DisplayName() : std::string("?");
        }

        // MC DebugScreenOverlay.formatKeybind: "[F3+A]".
        std::string FormatKeybind(const Input::KeyMapping* bind) {
            const Input::KeyMapping* mod = Input::Binds::DebugModifier;
            const std::string prefix = (mod && mod->key.IsBound()) ? KeyName(mod) + "+" : std::string();
            return "[" + prefix + KeyName(bind) + "]";
        }

        std::string FormatChart(const Input::KeyMapping* bind, const std::string& name, bool status) {
            return FormatKeybind(bind) + " " + name + " " + (status ? "visible" : "hidden");
        }
    }

    // ── Billboard text queue ────────────────────────────────────────────

    void QueueBillboardText(BillboardText text) {
        if (g_billboards.size() < 4096) g_billboards.push_back(std::move(text));
    }

    void ClearBillboardTexts() { g_billboards.clear(); }

    void RenderBillboardTexts(GuiGraphics& g, const float* projPtr, const float* viewPtr, int guiWidth, int guiHeight) {
        if (g_billboards.empty()) return;
        const glm::mat4 proj = glm::make_mat4(projPtr);
        const glm::mat4 view = glm::make_mat4(viewPtr);
        const glm::mat4 vp = proj * view;
        const float projY = proj[1][1];
        for (const BillboardText& t : g_billboards) {
            // `view` is the render-space view: the world position goes
            // through ToRender (double subtraction) before the projection.
            const glm::vec4 clip = vp * glm::vec4(Render::ToRender(glm::dvec3(t.x, t.y, t.z)), 1.0f);
            if (clip.w <= 0.0f) continue;
            const float sx = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(guiWidth);
            const float sy = (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * static_cast<float>(guiHeight);
            if (sx < -200.0f || sy < -50.0f || sx > guiWidth + 200.0f || sy > guiHeight + 50.0f) continue;
            // MC TextGizmo: 0.025 world units per font pixel, times the gizmo's scale.
            const float scale = (0.025f * t.scale * static_cast<float>(guiHeight) * projY) / (2.0f * clip.w);
            if (scale < 0.02f) continue;
            const int textW = g.GetStringWidth(t.text);
            g.PushMatrix();
            g.Translate(sx, sy);
            g.Scale(scale, scale);
            const int tx = t.centered ? -textW / 2 : 0;
            const int ty = -FontRenderer::LINE_HEIGHT / 2;
            g.DrawString(t.text, tx, ty, t.color, false);
            g.PopMatrix();
        }
        g_billboards.clear();
    }

    // ── DebugScreenOverlay ──────────────────────────────────────────────

    DebugScreenOverlay::DebugScreenOverlay()
        : m_frameTimeLogger(1),
          m_tickTimeLogger(TPS_DIMENSIONS),
          m_pingLogger(1),
          m_bandwidthLogger(1),
          m_fpsChart(m_frameTimeLogger),
          m_tpsChart(m_tickTimeLogger, [this] { return m_lastMspt; }),
          m_pingChart(m_pingLogger),
          m_bandwidthChart(m_bandwidthLogger) {}

    bool DebugScreenOverlay::ShowDebugScreen() const {
        const EntryList& entries = Entries();
        return entries.IsOverlayVisible() || !entries.GetCurrentlyEnabled().empty();
    }
    bool DebugScreenOverlay::ShowProfilerChart() const  { return Entries().IsOverlayVisible() && m_renderProfilerChart; }
    bool DebugScreenOverlay::ShowNetworkCharts() const  { return Entries().IsOverlayVisible() && m_renderNetworkCharts; }
    bool DebugScreenOverlay::ShowFpsCharts() const      { return Entries().IsOverlayVisible() && m_renderFpsCharts; }
    bool DebugScreenOverlay::ShowLightmapTexture() const { return Entries().IsOverlayVisible() && m_renderLightmapTexture; }

    void DebugScreenOverlay::ToggleNetworkCharts() {
        m_renderNetworkCharts = !Entries().IsOverlayVisible() || !m_renderNetworkCharts;
        if (m_renderNetworkCharts) {
            Entries().SetOverlayVisible(true);
            m_renderFpsCharts = false;
            m_renderLightmapTexture = false;
        }
    }

    void DebugScreenOverlay::ToggleFpsCharts() {
        m_renderFpsCharts = !Entries().IsOverlayVisible() || !m_renderFpsCharts;
        if (m_renderFpsCharts) {
            Entries().SetOverlayVisible(true);
            m_renderNetworkCharts = false;
            m_renderLightmapTexture = false;
        }
    }

    void DebugScreenOverlay::ToggleLightmapTexture() {
        m_renderLightmapTexture = !Entries().IsOverlayVisible() || !m_renderLightmapTexture;
        if (m_renderLightmapTexture) {
            Entries().SetOverlayVisible(true);
            m_renderFpsCharts = false;
            m_renderNetworkCharts = false;
        }
    }

    void DebugScreenOverlay::ToggleProfilerChart() {
        m_renderProfilerChart = !Entries().IsOverlayVisible() || !m_renderProfilerChart;
        if (m_renderProfilerChart) Entries().SetOverlayVisible(true);
    }

    void DebugScreenOverlay::Reset() {
        m_tickTimeLogger.Reset();
        m_pingLogger.Reset();
        m_bandwidthLogger.Reset();
    }

    void DebugScreenOverlay::RenderLines(GuiGraphics& g, const std::vector<std::string>& lines, bool alignLeft, int scaledScreenWidth) {
        const int height = FontRenderer::LINE_HEIGHT;
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& line = lines[i];
            if (line.empty()) continue;
            const int width = g.GetStringWidth(line);
            const int left = alignLeft ? MARGIN_LEFT : scaledScreenWidth - MARGIN_RIGHT - width;
            const int top = MARGIN_TOP + height * static_cast<int>(i);
            g.Fill(left - 1, top - 1, left + width + 1, top + height - 1, kShadeColor);
        }
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& line = lines[i];
            if (line.empty()) continue;
            const int width = g.GetStringWidth(line);
            const int left = alignLeft ? MARGIN_LEFT : scaledScreenWidth - MARGIN_RIGHT - width;
            const int top = MARGIN_TOP + height * static_cast<int>(i);
            g.DrawString(line, left, top, kTextColor, false);
        }
    }

    // MC's lightmap.fsh evaluated on the CPU, fed the same inputs the
    // LightTexture would supply from this world: the sky darken curve of
    // the current time of day, no flicker, the dimension's ambient light
    // and the brightness option.
    void DebugScreenOverlay::RenderLightmap(GuiGraphics& g, int x, int y, const Context&) {
        static TextureHandle s_texture = INVALID_TEXTURE;
        if (!Render::g_renderBackend) return;
        // MC Level.getTimeOfDay / getSkyDarken.
        const double dayTime = static_cast<double>(Render::EnvironmentState::Get().DayTime() % 24000);
        double frac = dayTime / 24000.0 - 0.25;
        if (frac < 0.0) frac += 1.0;
        if (frac > 1.0) frac -= 1.0;
        const double d = 1.0 - (std::cos(frac * 3.141592653589793) + 1.0) / 2.0;
        const double timeOfDay = frac + (d - frac) / 3.0;
        double darken = 1.0 - (std::cos(timeOfDay * 6.283185307179586) * 2.0 + 0.2);
        darken = std::clamp(darken, 0.0, 1.0);
        darken = 1.0 - darken;
        const float skyFactor = static_cast<float>(darken * 0.8 + 0.2);
        const float blockFactor = 1.5f;   // blockLightRedFlicker (0) + 1.5
        const Game::DimensionId dim = Client::ClientLevels::ActiveDimension();
        const float ambient = dim == Game::DimensionId::Overworld ? 0.0f : 0.1f;
        const glm::vec3 ambientColor(ambient);
        const glm::vec3 skyLightColor = dim == Game::DimensionId::End
            ? glm::mix(glm::vec3(skyFactor, skyFactor, 1.0f), glm::vec3(0.99f, 1.12f, 1.0f), 0.35f) / std::max(skyFactor, 0.001f)
            : glm::vec3(1.0f);
        const glm::vec3 blockLightTint(1.0f, 0.82f, 0.55f);
        const float brightness = std::clamp(Platform::g_gameSettings.GetGamma(), 0.0f, 1.0f);

        auto getBrightness = [](float level) { return level / (4.0f - 3.0f * level); };
        auto notGamma = [](glm::vec3 c) {
            const float maxComponent = std::max(std::max(c.x, c.y), c.z);
            if (maxComponent <= 0.0f) return c;
            const float inv = 1.0f - maxComponent;
            const float scaled = 1.0f - inv * inv * inv * inv;
            return c * (scaled / maxComponent);
        };
        uint8_t pixels[16 * 16 * 4];
        for (int sy = 0; sy < 16; ++sy) {
            for (int bx = 0; bx < 16; ++bx) {
                const float blockLevel = static_cast<float>(bx) / 15.0f;
                const float skyLevel = static_cast<float>(sy) / 15.0f;
                const float blockBrightness = getBrightness(blockLevel) * blockFactor;
                const float skyBrightness = getBrightness(skyLevel) * skyFactor;
                glm::vec3 color = ambientColor;
                color += skyLightColor * skyBrightness;
                const float parabolic = (2.0f * blockLevel - 1.0f) * (2.0f * blockLevel - 1.0f);
                const glm::vec3 blockColor = glm::mix(blockLightTint, glm::vec3(1.0f), 0.9f * parabolic);
                color += blockColor * blockBrightness;
                color = glm::clamp(color, 0.0f, 1.0f);
                color = glm::mix(color, notGamma(color), brightness);
                // MC's texture has sky light on Y going DOWN (row 0 = sky 0),
                // and the overlay blits it flipped (v 1→0), so row 0 lands at
                // the bottom. Store rows top-down as sky 15..0 to match.
                uint8_t* px = pixels + ((15 - sy) * 16 + bx) * 4;
                px[0] = static_cast<uint8_t>(std::lround(color.x * 255.0f));
                px[1] = static_cast<uint8_t>(std::lround(color.y * 255.0f));
                px[2] = static_cast<uint8_t>(std::lround(color.z * 255.0f));
                px[3] = 255;
            }
        }
        if (s_texture == INVALID_TEXTURE) {
            s_texture = Render::g_renderBackend->CreateTexture2D(16, 16, TextureFormat::RGBA8, pixels);
            Render::g_renderBackend->SetTextureFilter(s_texture, TextureFilter::Nearest, TextureFilter::Nearest);
        } else {
            Render::g_renderBackend->UpdateTexture2D(s_texture, 0, 0, 16, 16, pixels);
        }
        g.Fill(x - 1, y - 1, x + 64 + 1, y + 64 + 1, 0xFF000000);
        g.Blit(s_texture, x, y, x + 64, y + 64, 0.0f, 0.0f, 1.0f, 1.0f);
    }

    // MC LevelLoadingScreen.extractChunksForRendering over the server's
    // ChunkLoadStatusView, coloured by status.
    void DebugScreenOverlay::RenderServerChunkMap(GuiGraphics& g, int xCenter, int yCenter) {
        if (!Server::g_integratedServer) return;
        const auto sample = Server::g_integratedServer->GetDebugSample();
        if (!sample.valid || sample.statusRadius <= 0) return;
        const int size = 4, margin = 1;
        const int width = size + margin;
        const int diameter = sample.statusRadius * 2 + 1;
        const int totalWidth = diameter * width - margin;
        const int xStart = xCenter - totalWidth / 2;
        const int yStart = yCenter - totalWidth / 2;
        const int half = width / 2 + 1;
        g.Fill(xCenter - half, yCenter - half, xCenter + half, yCenter + half, 0xFFFF0000);
        auto colorFor = [](uint8_t raw) -> uint32_t {
            switch (static_cast<Server::ChunkStatus>(raw)) {
                case Server::ChunkStatus::EMPTY:         return 0xFF545454;   // ChunkStatus.EMPTY
                case Server::ChunkStatus::LOADING:       return 0xFF999999;   // STRUCTURE_STARTS
                case Server::ChunkStatus::GENERATING:    return 0xFF80B252;   // BIOMES
                case Server::ChunkStatus::STRUCTURE_GEN: return 0xFF5F6191;   // STRUCTURE_REFERENCES
                case Server::ChunkStatus::FEATURES:      return 0xFF21C600;   // FEATURES
                case Server::ChunkStatus::LIGHT_GEN:     return 0xFFCCCCCC;   // INITIALIZE_LIGHT
                case Server::ChunkStatus::LIGHT_READY:   return 0xFFFFE7A0;   // LIGHT
                case Server::ChunkStatus::FULL:          return 0xFFFFFFFF;   // FULL
                default:                                 return 0xFFAA0000;
            }
        };
        for (int z = 0; z < diameter; ++z) {
            for (int x = 0; x < diameter; ++x) {
                const size_t idx = static_cast<size_t>(z) * diameter + x;
                if (idx >= sample.chunkStatus.size()) continue;
                const int cx = xStart + x * width;
                const int cy = yStart + z * width;
                g.Fill(cx, cy, cx + size, cy + size, colorFor(sample.chunkStatus[idx]));
            }
        }
    }

    void DebugScreenOverlay::Render(GuiGraphics& g, const Context& ctx, int standardGuiScale) {
        EntryList& entries = Entries();
        const std::vector<std::string> visible = entries.GetCurrentlyEnabled();
        if (visible.empty()) return;

        // MC's anonymous DebugScreenDisplayer.
        std::vector<std::string> leftLines, rightLines, regularLines;
        std::vector<std::pair<std::string, std::vector<std::string>>> groups;
        struct Collector : Displayer {
            std::vector<std::string>& left; std::vector<std::string>& right; std::vector<std::string>& regular;
            std::vector<std::pair<std::string, std::vector<std::string>>>& groups;
            Collector(std::vector<std::string>& l, std::vector<std::string>& r, std::vector<std::string>& reg,
                      std::vector<std::pair<std::string, std::vector<std::string>>>& gr)
                : left(l), right(r), regular(reg), groups(gr) {}
            std::vector<std::string>& Group(const std::string& name) {
                for (auto& [n, lines] : groups) if (n == name) return lines;
                groups.emplace_back(name, std::vector<std::string>{});
                return groups.back().second;
            }
            void AddPriorityLine(std::string line) override {
                if (left.size() > right.size()) right.push_back(std::move(line));
                else left.push_back(std::move(line));
            }
            void AddLine(std::string line) override { regular.push_back(std::move(line)); }
            void AddToGroup(const std::string& group, std::vector<std::string> lines) override {
                auto& g = Group(group);
                g.insert(g.end(), std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
            }
            void AddToGroup(const std::string& group, std::string line) override { Group(group).push_back(std::move(line)); }
        } displayer(leftLines, rightLines, regularLines, groups);

        for (const std::string& id : visible) {
            if (Entry* e = GetEntry(id)) e->Display(displayer, ctx);
        }

        if (!leftLines.empty()) leftLines.emplace_back();
        if (!rightLines.empty()) rightLines.emplace_back();
        if (!regularLines.empty()) {
            const size_t mid = (regularLines.size() + 1) / 2;
            leftLines.insert(leftLines.end(), regularLines.begin(), regularLines.begin() + static_cast<long>(mid));
            rightLines.insert(rightLines.end(), regularLines.begin() + static_cast<long>(mid), regularLines.end());
            leftLines.emplace_back();
            if (mid < regularLines.size()) rightLines.emplace_back();
        }
        if (!groups.empty()) {
            const size_t mid = (groups.size() + 1) / 2;
            for (size_t i = 0; i < groups.size(); ++i) {
                const auto& lines = groups[i].second;
                if (lines.empty()) continue;
                if (i < mid) { leftLines.insert(leftLines.end(), lines.begin(), lines.end()); leftLines.emplace_back(); }
                else         { rightLines.insert(rightLines.end(), lines.begin(), lines.end()); rightLines.emplace_back(); }
            }
        }

        if (entries.IsOverlayVisible()) {
            leftLines.emplace_back();
            const bool hasServer = !ctx.isRemoteClient && Server::g_integratedServer != nullptr;
            leftLines.push_back("Debug charts: " + FormatChart(Input::Binds::DebugProfilingChart, "Profiler", m_renderProfilerChart) + "; " +
                                FormatChart(Input::Binds::DebugFpsCharts, hasServer ? "fps + tps" : "fps", m_renderFpsCharts) + ";");
            leftLines.push_back(FormatChart(Input::Binds::DebugNetworkCharts, ctx.isRemoteClient ? "Bandwidth + Ping" : "Ping", m_renderNetworkCharts) +
                                "; " + FormatChart(Input::Binds::DebugLightmapTexture, "Lightmap", m_renderLightmapTexture));
            leftLines.push_back("To edit: press " + FormatKeybind(Input::Binds::DebugDebugOptions));
        }

        // MC: the "Debug GUI Scale" option may draw the overlay smaller than
        // the rest of the GUI. -1 = unchanged, 0 = auto/2, n = that scale.
        int newScale = Platform::g_gameSettings.GetInt("debugGuiScale", -1);
        if (newScale == -1) newScale = standardGuiScale;
        else if (newScale == 0) newScale = CalculateScale(0, ctx.framebufferWidth, ctx.framebufferHeight) / 2;
        else newScale = CalculateScale(newScale, ctx.framebufferWidth, ctx.framebufferHeight);

        g.PushMatrix();
        int scaledScreenWidth, scaledScreenHeight;
        if (newScale < standardGuiScale && newScale > 0) {
            g.Scale(static_cast<float>(newScale) / static_cast<float>(standardGuiScale),
                    static_cast<float>(newScale) / static_cast<float>(standardGuiScale));
            scaledScreenWidth = ctx.framebufferWidth / newScale;
            scaledScreenHeight = ctx.framebufferHeight / newScale;
        } else {
            scaledScreenWidth = g.GuiWidth();
            scaledScreenHeight = g.GuiHeight();
        }

        RenderLines(g, leftLines, true, scaledScreenWidth);
        RenderLines(g, rightLines, false, scaledScreenWidth);
        g.NextStratum();

        m_profilerPieChart.SetBottomOffset(10);
        if (ShowFpsCharts()) {
            const int maxWidth = scaledScreenWidth / 2;
            m_fpsChart.SetFramerateLimit(ctx.maxFps);
            m_fpsChart.Render(g, 0, m_fpsChart.GetWidth(maxWidth), scaledScreenHeight);
            if (m_tickTimeLogger.Size() > 0) {
                if (Server::g_integratedServer) {
                    const auto s = Server::g_integratedServer->GetDebugSample();
                    if (s.valid) m_lastMspt = s.msPerTick;
                }
                const int width = m_tpsChart.GetWidth(maxWidth);
                m_tpsChart.Render(g, scaledScreenWidth - width, width, scaledScreenHeight);
            }
            m_profilerPieChart.SetBottomOffset(m_tpsChart.GetFullHeight());
        }
        if (ShowNetworkCharts() && Client::g_networkClient && Client::g_networkClient->IsConnected()) {
            const int maxWidth = scaledScreenWidth / 2;
            if (ctx.isRemoteClient) {
                m_bandwidthChart.Render(g, 0, m_bandwidthChart.GetWidth(maxWidth), scaledScreenHeight);
            }
            const int width = m_pingChart.GetWidth(maxWidth);
            m_pingChart.Render(g, scaledScreenWidth - width, width, scaledScreenHeight);
            m_profilerPieChart.SetBottomOffset(m_pingChart.GetFullHeight());
        }
        if (ShowLightmapTexture()) {
            RenderLightmap(g, scaledScreenWidth - 64 - 2, scaledScreenHeight - 64 - 2, ctx);
        }
        if (entries.IsCurrentlyEnabled(Ids::VisualizeChunksOnServer) && !ctx.isRemoteClient) {
            RenderServerChunkMap(g, scaledScreenWidth / 2, scaledScreenHeight / 2);
        }
        if (ShowProfilerChart()) {
            m_profilerPieChart.SetPieChartResults(&m_profileResults);
            m_profilerPieChart.Render(g, scaledScreenWidth, scaledScreenHeight);
        }
        g.PopMatrix();
    }

    DebugScreenOverlay& Overlay() {
        static DebugScreenOverlay s_overlay;
        return s_overlay;
    }

} // namespace Render::DebugScreen
