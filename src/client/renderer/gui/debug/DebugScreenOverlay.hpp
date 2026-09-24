// File: src/client/renderer/gui/debug/DebugScreenOverlay.hpp
//
// The F3 screen — a port of MC 26.3's DebugScreenOverlay. Draws the two
// text columns from the enabled entries (DebugScreenEntries), the F3+1
// profiler pie, the F3+2 frame/tick charts, the F3+3 ping/bandwidth charts,
// the F3+4 lightmap and the server chunk map. Rendered through the HUD's
// GuiGraphics from PlatformMain::RenderHUD, after the HUD proper.
//
// Lives in GAME_SOURCES (not the imgui target) so the entries can read the
// world, the server sample and the network client directly.
#pragma once

#include "DebugScreenEntries.hpp"
#include "DebugCharts.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Render {

    class GuiGraphics;

    namespace DebugScreen {

        class DebugScreenOverlay {
        public:
            DebugScreenOverlay();

            // Draw everything the overlay owns. `standardGuiScale` is the
            // HUD's scale (MC Window.getGuiScale) — the "Debug GUI Scale"
            // option may shrink the overlay below it.
            void Render(GuiGraphics& g, const Context& ctx, int standardGuiScale);

            // Text billboards the debug renderers queued this frame
            // (Gizmos::BillboardText) — drawn here so they use the HUD font.
            bool ShowDebugScreen() const;
            bool ShowProfilerChart() const;
            bool ShowNetworkCharts() const;
            bool ShowFpsCharts() const;
            bool ShowLightmapTexture() const;

            void ToggleNetworkCharts();
            void ToggleFpsCharts();
            void ToggleLightmapTexture();
            void ToggleProfilerChart();

            // Per-frame / per-tick feeds from PlatformMain.
            void LogFrameDuration(int64_t nanos) { m_frameTimeLogger.LogSample(nanos); }
            LocalSampleLogger& TickTimeLogger()  { return m_tickTimeLogger; }
            LocalSampleLogger& PingLogger()      { return m_pingLogger; }
            LocalSampleLogger& BandwidthLogger() { return m_bandwidthLogger; }
            ProfilerPieChart&  PieChart()        { return m_profilerPieChart; }
            void SetProfileResults(ProfileNode root) { m_profileResults.SetRoot(std::move(root)); }
            // Digit keys while the pie chart is up (MC profilerPieChartKeyPress).
            void ProfilerKeyPress(int digit) { m_profilerPieChart.KeyPress(digit); }

            // Session teardown (MC DebugScreenOverlay.reset).
            void Reset();

        private:
            void RenderLines(GuiGraphics& g, const std::vector<std::string>& lines, bool alignLeft, int scaledScreenWidth);
            void RenderLightmap(GuiGraphics& g, int x, int y, const Context& ctx);
            void RenderServerChunkMap(GuiGraphics& g, int xCenter, int yCenter);

            bool m_renderProfilerChart = false;
            bool m_renderFpsCharts = false;
            bool m_renderNetworkCharts = false;
            bool m_renderLightmapTexture = false;

            LocalSampleLogger m_frameTimeLogger;
            LocalSampleLogger m_tickTimeLogger;
            LocalSampleLogger m_pingLogger;
            LocalSampleLogger m_bandwidthLogger;
            FpsDebugChart       m_fpsChart;
            TpsDebugChart       m_tpsChart;
            PingDebugChart      m_pingChart;
            BandwidthDebugChart m_bandwidthChart;
            ProfilerPieChart    m_profilerPieChart;
            ProfileResults      m_profileResults;
            float m_lastMspt = 50.0f;
        };

        DebugScreenOverlay& Overlay();   // the one instance (MC Minecraft.getDebugOverlay())

        // The debug renderers' text billboards (MC Gizmos.billboardText):
        // world-space anchors drawn in GUI space by the overlay, the way the
        // player nametags are.
        struct BillboardText {
            // WORLD position, double: projected under the main view's
            // render origin at HUD time (RenderOrigin.hpp).
            double x, y, z;
            std::string text;
            uint32_t color;
            float scale;       // MC TextGizmo scale (1 = nametag size)
            bool centered;
            bool alwaysOnTop;
        };
        void QueueBillboardText(BillboardText text);
        // Draw and clear the queue. proj/view are the main view's matrices.
        void RenderBillboardTexts(GuiGraphics& g, const float* proj, const float* view, int guiWidth, int guiHeight);
        void ClearBillboardTexts();

    } // namespace DebugScreen
} // namespace Render
